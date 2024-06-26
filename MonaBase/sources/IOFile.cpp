/*
This file is a part of MonaSolutions Copyright 2017
mathieu.poux[a]gmail.com
jammetthomas[a]gmail.com

This program is free software: you can redistribute it and/or
modify it under the terms of the the Mozilla Public License v2.0.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
Mozilla Public License v. 2.0 received along this program for more
details (or else see http://mozilla.org/MPL/2.0/).

*/

#include "Mona/IOFile.h"
#include <list>

using namespace std;

namespace Mona {

struct IOFile::Action : Runner, virtual Object {
	Action(const char* name, const Handler& handler, const shared<File>& pFile) : Runner(name), _pFile(pFile) {
		pFile->_pHandler = &handler;
	}

	struct Handle : Runner, virtual Object {
		Handle(const char* name, shared<File>& pFile) : Runner(name), _weakFile(pFile) {
			// reset pFile to release its usage by parallel thread: to avoid handling fo nothing
			pFile.reset();
		}
	private:
		bool run(Exception& ex) {
			shared<File> pFile = _weakFile.lock();
			if(pFile)
				handle(*pFile);
			return true;
		}
		virtual void handle(File& file) = 0;
		weak<File>	_weakFile;
	};

protected:
	template<typename HandleType, typename ...Args>
	void handle(Args&&... args) {
		if(!_pFile.unique()) // else useless!
			_pFile->_pHandler->queue<HandleType>(name, _pFile, forward<Args>(args)...);
	}

private:
	bool run(Exception& ex) {
		if (process(ex, _pFile) || !_pFile)
			return true;
		// ErrorHandle has to keep a handler on pFile to avoid main thread to continue operation
		// (shared<File> must stay not unique until main thread error reception)
		struct ErrorHandle : Runner, virtual Object {
			ErrorHandle(const char* name, shared<File>& pFile, Exception& ex) : Runner(name), _pFile(move(pFile)), _ex(move(ex)) {}
		private:
			bool run(Exception& ex) {
				_pFile->_onError(_ex);
				return true;
			}
			shared<File>	_pFile;
			Exception		_ex;
		};
		handle<ErrorHandle>(ex);
		return true;
	}
	virtual bool process(Exception& ex, shared<File>& pFile) {
		if (pFile->load(ex))
			return true;
		if (pFile->mode != File::MODE_DELETE)
			return false;
		// no error on opening en mode deletion!
		ex = nullptr;
		return true;
	}

	shared<File> _pFile;
};

IOFile::IOFile(const Handler& handler, const ThreadPool& threadPool, UInt16 cores) :
	handler(handler), threadPool(threadPool), _threadPool(Thread::PRIORITY_LOW, cores*2), Thread("FileWatching") { // 2*CPU => because disk speed can be at maximum 2x more than memory, and Low priority to not impact main thread pool
}

IOFile::~IOFile() {
	join();
	stop(); // file watchers!
}

void IOFile::join() {
	// join devices (reading and writing operation)	
	do {
		((ThreadPool&)threadPool).join(); // wait possible decoding (can cast because IOFile constructor takes a non-const threadPool object)
	} while(_threadPool.join()); // while reading/writing operation
}

void IOFile::subscribe(const shared<File>& pFile, const File::OnError& onError, const File::OnFlush& onFlush) {
	pFile->_onError = onError;
	if ((pFile->_onFlush = onFlush) && (pFile->mode==File::MODE_WRITE|| pFile->mode == File::MODE_APPEND))
		onFlush(false); // can start write operation immediatly!
}

void IOFile::load(const shared<File>& pFile) {
	if (*pFile)
		return;
	_threadPool.queue<Action>(pFile->_ioTrack, "LoadFile", handler, pFile);
}

void IOFile::read(const shared<File>& pFile, UInt32 size) {
	struct ReadFile : Action {
		ReadFile(const Handler& handler, const shared<File>& pFile, const ThreadPool& threadPool, UInt32 size) : Action("ReadFile", handler, pFile), _threadPool(threadPool), _size(size) {}
	private:
		struct Handle : Action::Handle, virtual Object {
			Handle(const char* name, shared<File>& pFile, shared<Buffer>& pBuffer, bool end) :
				Action::Handle(name, pFile), _pBuffer(move(pBuffer)), _end(end) {}
		private:
			void handle(File& file) { file._onReaden(_pBuffer, _end); }
			shared<Buffer>	_pBuffer;
			bool   _end;
		};
		bool process(Exception& ex, shared<File>& pFile) override {
			if (pFile.unique())
				return true; // useless to read here, nobody to receive it!
			// take the required size just if not exceeds file size to avoid to allocate a too big buffer (expensive)
			// + use pFile->size() without refreshing to use as same size as caller has gotten it (for example to write a content-length in header)
			UInt64 available = pFile->size() - pFile->readen();
			shared<Buffer>	pBuffer(SET, UInt32(min(available, _size)));
			pBuffer.set(UInt32(min(available, _size)));
			int readen = pFile->read(ex, pBuffer->data(), pBuffer->size());
			if (readen < 0)
				return false;
			if ((_size=readen) < pBuffer->size())
				pBuffer->resize(readen, true);
			if (pFile->_pDecoder) {
				struct Decoding : Action, virtual Object {
					Decoding(shared<File>& pFile, const ThreadPool& threadPool, shared<Buffer>& pBuffer, bool end) :
						_pThread(ThreadQueue::Current()), _threadPool(threadPool), _end(end), Action("DecodingFile", *pFile->_pHandler, pFile), _pBuffer(move(pBuffer)) {
						pFile.reset();
					}
				private:
					bool process(Exception& ex, shared<File>& pFile) override {
						if (pFile.unique())
							return true; // useless to decode here, nobody to receive it!
						UInt32 decoded = pFile->_pDecoder->decode(_pBuffer, _end);
						// decoded=wantToRead!
						if(decoded && !_end)
							_pThread->queue<ReadFile>(*pFile->_pHandler, pFile, _threadPool, decoded);
						if (_pBuffer)
							handle<ReadFile::Handle>(_pBuffer, _end);
						return true;
					}
					shared<Buffer>		_pBuffer;
					bool				_end;
					const ThreadPool&	_threadPool;
					ThreadQueue*		_pThread;
				};
				_threadPool.queue<Decoding>(pFile->_decodingTrack, pFile, _threadPool, pBuffer, _size == available);
			} else
				handle<Handle>(pBuffer, _size == available);
			return true;
		}
		UInt32				_size;
		const ThreadPool&	_threadPool;
	};
	// always do the job even if size==0 to get a onReaden event!
	_threadPool.queue<ReadFile>(pFile->_ioTrack, handler, pFile, threadPool, size);
}

void IOFile::write(const shared<File>& pFile, const Packet& packet) {
	struct WriteFile : Action { 
		WriteFile(const Handler& handler, const shared<File>& pFile, const Packet& packet) : _packet(move(packet)), Action("WriteFile", handler, pFile) {
			pFile->_queueing += _packet.size();
		}
	private:
		struct Handle : Action::Handle, virtual Object {
			Handle(const char* name, shared<File>& pFile) : Action::Handle(name, pFile) {}
		private:
			void handle(File& file) {
				if(!--file._flushing)
					file._onFlush(!file.loaded());
			}
		};
		bool process(Exception& ex, shared<File>& pFile) override {
			// No check pFile.unique => File writing full asynchronous (without any other hand on the file)
			UInt64 queueing = (pFile->_queueing -= _packet.size());
			if (!pFile->write(ex, _packet.data(), _packet.size()))
				return false;
			if (queueing)
				return true;
			if(!pFile->_flushing++) // To signal end of write!
				handle<Handle>();
			else
				--pFile->_flushing;
			return true;
		}
		Packet		 _packet;
	};
	// do the WriteFile even if packet is empty when not loaded to allow to open the file and clear its content or create the file
	// or to allow to create the folder => if File is a Folder opened in WRITE/APPEND mode loaded is always false and write an empty packet create the folder => allow a folder creation asynchrone!
	if(packet || !pFile->loaded())
		_threadPool.queue<WriteFile>(pFile->_ioTrack, handler, pFile, packet);
}

void IOFile::erase(const shared<File>& pFile) {
	struct EraseFile : Action {
		EraseFile(const Handler& handler, const shared<File>& pFile) : Action("EraseFile", handler, pFile) {}
	private:
		struct Handle : Action::Handle, virtual Object {
			Handle(const char* name, shared<File>& pFile) : Action::Handle(name, pFile) {}
		private:
			void handle(File& file) {
				if (!--file._flushing)
					file._onFlush(!file.loaded());
			}
		};
		bool process(Exception& ex, shared<File>& pFile) override {
			// No check pFile.unique => File erasing full asynchronous (without any other hand on the file)
			if (!pFile->erase(ex))
				return false;
			if (!pFile->_flushing++) // To signal end of write!
				handle<Handle>();
			else
				--pFile->_flushing;
			return true;
		}
	};
	_threadPool.queue<EraseFile>(pFile->_ioTrack, handler, pFile);
}


void IOFile::watch(const shared<const FileWatcher>& pFileWatcher, const FileWatcher::OnUpdate& onUpdate) {
	lock_guard<mutex> lock(_mutexWatchers);
	_watchers.emplace_back(pFileWatcher);
	struct OnUpdate : Runner, virtual Object {
		OnUpdate(const FileWatcher::OnUpdate& onUpdate, const Path& file, bool firstWatch) : _onUpdate(onUpdate), _file(file), _firstWatch(firstWatch), Runner("WatchUpdating") {}
		bool run(Exception& ex) { _onUpdate(_file, _firstWatch); return true; }
	private:
		FileWatcher::OnUpdate	_onUpdate;
		bool					_firstWatch;
		Path					_file;
	};
	((FileWatcher&)*_watchers.back()).onUpdate = [this, onUpdate](const Path& file, bool firstWatch) {
		handler.queue<OnUpdate>(onUpdate, file, firstWatch);
	};
	start(Thread::PRIORITY_LOWEST);
}

bool IOFile::run(Exception& ex, const volatile bool& requestStop) {
	list<shared<const FileWatcher>> watchers;
	while(!requestStop) {
		Time time;
		{
			lock_guard<mutex> lock(_mutexWatchers);
			watchers.insert(watchers.end(), _watchers.begin(), _watchers.end());
			_watchers.clear();
			if (watchers.empty()) {
				stop(); // to set _stop immediatly!
				break;
			}
		}
		auto it = watchers.begin();
		while (it != watchers.end()) {
			if (it->unique()) {
				it = watchers.erase(it);
				continue;
			}
			const shared<const FileWatcher> pWatcher = *it++;
			AUTO_ERROR(((FileWatcher&)*pWatcher).watch(ex, pWatcher->onUpdate)>=0, "File watching");
			ex = nullptr;
		}
		if (wakeUp.wait((UInt32)max(1000 - time.elapsed(), 1)))
			break;// wait()==true means requestStop=true because there is no other wakeUp.set elsewhere
	}

	for (const shared<const FileWatcher>& pWatcher : watchers) {
		if (!pWatcher.unique()) {
			ex.set<Ex::Intern>("Some file watcher are still active while IOFile is deleting");
			return false;
		}
	}
	return true;
}

} // namespace Mona
