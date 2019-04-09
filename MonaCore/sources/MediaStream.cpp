/*
This file is a part of MonaSolutions Copyright 2017
mathieu.poux[a]gmail.com
jammetthomas[a]gmail.com

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License received along this program for more
details (or else see http://www.gnu.org/licenses/).

*/

#include "Mona/MediaStream.h"
#include "Mona/MediaSocket.h"
#include "Mona/MediaFile.h"
#include "Mona/MediaServer.h"

using namespace std;


namespace Mona {

MediaStream::MediaStream(Type type, const Path& path, Media::Source& source) :
	_starting(false), _running(false), targets(_targets), _startCount(0),
	type(type), path(path), source(source) {
}
void MediaStream::start(const Parameters& parameters) {
	ex = nullptr; // reset lastEx on pullse start!
	if (_running && !_starting)
		return; // nothing todo, starting already done!
	if (!_startCount && !_running) {
		// do it just on first start call!
		Media::Target* pTarget = dynamic_cast<Media::Target*>(this);
		if (pTarget) {
			// This is a target, with beginMedia it can be start, so create a _pTarget and keep it alive all the life-time of this Stream
			// Indeed it can be stopped and restarted with beginMedia!
			_pTarget = shared<Media::Target>(make_shared<Media::Target>(), pTarget); // aliasing
			onNewTarget(_pTarget);
		}
	}
	_starting = true;
	if (starting(parameters))
		finalizeStart();
	if (_starting) // _starting can switch to false if finalizeStart (then _running is already set to true) and on stop (then _running must stay on false)
		_running = true;
}
bool MediaStream::finalizeStart() {
	if (!_starting)
		return false;
	if (!_running) // called while starting!
		_running = true;
	if (onStart && !onStart()) {
		stop();
		return false;
	}
	_starting = false;
	++_startCount;
	INFO(description(), " starts");
	return true;
}
void MediaStream::stop() {
	if (!_running && !_starting)
		return;
	stopping();
	_running = false;
	_targets.clear(); // to invalid targets!
	if (_starting) {
		_starting = false;
		return;
	}
	INFO(description(), " stops");
	onStop(); // in last to allow possibily a delete this (beware impossible with Subscription usage!)
}
shared<const Socket> MediaStream::socket() const {
	if (type>0)
		WARN(typeof(self), " should implement socket()");
	return nullptr;
}
shared<const File> MediaStream::file() const {
	if (type == TYPE_FILE)
		WARN(typeof(self), " should implement file()");
	return nullptr;
}

shared<Socket> MediaStream::newSocket(const Parameters& parameters, const shared<TLS>& pTLS) {
	if (type <= 0)
		return nullptr;
	shared<Socket> pSocket;
	if (type == TYPE_SRT) {
#if defined(SRT_API)
		pSocket.set<SRT::Socket>();
		return pSocket;
#endif
		ERROR(description(), "SRT unsupported replacing by UDP (build MonaBase with SRT support before)");
	}
	if (pTLS)
		pSocket.set<TLS::Socket>(type == TYPE_UDP ? Socket::TYPE_DATAGRAM : Socket::TYPE_STREAM, pTLS);
	else
		pSocket.set(type == TYPE_UDP ? Socket::TYPE_DATAGRAM : Socket::TYPE_STREAM);
	Exception ex;
	AUTO_ERROR(pSocket->processParams(ex, parameters, "stream"), description());
	return pSocket;
}

unique<MediaStream> MediaStream::New(Exception& ex, Media::Source& source, const string& description, const Timer& timer, IOFile& ioFile, IOSocket& ioSocket, const shared<TLS>& pTLS) {
	// Net => [address] [type/TLS][/MediaFormat] [parameter]
	// File = > file[.format][MediaFormat][parameter]
	
	const char* line = String::TrimLeft(description.c_str());

	bool isTarget(&source==&Media::Source::Null());
	bool isBind = *line == '@';
	if (isBind)
		++line;

	// remove "" or ''
	string first;
	if (*line == '"' || *line =='\'') {
		const char* end = strchr(line +1, *line);
		if (end) {
			++line;
			first.assign(line, end - line);
			line = end +1;
		}
	}

	// isolate first (and remove blank)
	size_t size = 0;
	for(;;) {
		switch (line[size]) {
			default:
				++size;
				continue;
			case ' ':
			case '\t':
			case 0:;
		}
		first.append(line, size);
		line += size;
		break;
	}

	// query => parameters
	string query;
	size_t queryPos = first.find('?');
	if (queryPos != string::npos) {
		query.assign(first.c_str()+queryPos, first.size() - queryPos);
		first.resize(queryPos);
	}

	Type type(TYPE_FILE);
	bool isSecure(false);
	bool isFile(false);
	string format;
	String::ForEach forEach([&](UInt32 index, const char* value) {
		if (String::ICompare(value, "UDP") == 0) {
			isFile = false;
			type = TYPE_UDP;
			return true;
		}
		if (String::ICompare(value, "TCP") == 0) {
			isFile = false;
			if (type != TYPE_HTTP)
				type = TYPE_TCP;
			return true;
		}
		if (String::ICompare(value, "SRT") == 0) {
			isFile = false;
			type = TYPE_SRT;
			return true;
		}
		if (String::ICompare(value, "HTTP") == 0) {
			isFile = false;
			type = TYPE_HTTP;
			return true;
		}
		if (String::ICompare(value, "TLS") == 0) {
			isSecure = true;
			return true;
		}
		if (String::ICompare(value, "FILE") == 0) {
			// force file!
			isFile = true;
			type = TYPE_FILE;
			return true;
		}
		format = value;
		return false;
	});

	const char* params = strpbrk(line = String::TrimLeft(line), " \t\r\n\v\f");
	String::Split(line, params ? (line - params) : string::npos, "/", forEach, SPLIT_IGNORE_EMPTY);

#if !defined(SRT_API)
	if (type == TYPE_SRT) {
		ex.set<Ex::Unsupported>(TypeToString(type), " stream not supported, build MonaBase with SRT support first");
		return nullptr;
	}
#endif
	
	Path   path;
	SocketAddress address;
	UInt16 port;
	
	if (!isFile) {
		// if is not explicitly a file, test if it's a port
		size = first.find_first_of("/\\", 0);
		if (size == string::npos)
			size = first.size(); // to fix past.set (doesn't work with string::npos)
		if (String::ToNumber(first.data(), size, port)) {
			address.setPort(port);
			path.set(first.c_str() + size);
			if (!isBind && ((type != TYPE_UDP && type != TYPE_SRT) || isTarget)) // if no host and TCP/HTTP or target
				address.host().set(IPAddress::Loopback());
		} else {
			bool isAddress = false;
			// Test if it's an address
			{
				String::Scoped scoped(first.data() + size);
				Exception exc;
				isAddress = address.set(exc, first.data());
				if (!isAddress && type) {
					// explicitly indicate as network, and however address invalid!
					ex = exc;
					return nullptr;
				}
			}
			if (isAddress) {
				path.set(first.c_str() + size);
				if (!isBind && !address.host() && ((type != TYPE_UDP && type != TYPE_SRT) || isTarget)) {
					ex.set<Ex::Net::Address::Ip>("Wildcard binding impossible for a stream ", (isTarget ? "target " : "source "), TypeToString(type));
					return nullptr;
				}
			} else {
				if (!path.set(move(first))) {
					ex.set<Ex::Format>("No file name in stream file description");
					return nullptr;
				}
				if (path.isFolder()) {
					ex.set<Ex::Format>("Stream file ", path, " can't be a folder");
					return nullptr;
				}
				isFile = true;
				type = TYPE_FILE;
			}
		}
	}

	// fix params!
	if (params) {
		while (isspace(*params)) {
			if (!*++params) {
				params = NULL;
				break;
			}
		}
	}

	if (format.empty()) {
		switch (type) {
			case TYPE_SRT: // SRT and No Format => TS by default to catch Haivision usage
			case TYPE_UDP: // UDP and No Format => TS by default to catch with VLC => UDP = TS
				format = "mp2t";
				break;
			case TYPE_HTTP:
				if (path.isFolder()) {
					ex.set<Ex::Format>("A HTTP source or target stream can't be a folder");
					return nullptr;
				}
			default: {
				const char* subMime;
				if (MIME::Read(path, subMime)) {
					format = subMime;
					break;
				}
				if (!isTarget && type == TYPE_HTTP)
					break; // HTTP source allow empty format (will be determinate with content-type)
				if(path.extension().empty())
					ex.set<Ex::Format>(TypeToString(type), " stream description have to indicate a media format");
				else
					ex.set<Ex::Format>(TypeToString(type), " stream path has a format ", path.extension(), " unknown or not supported");
				return nullptr;
			}
		}
	}
	
	unique<MediaStream> pStream;
	if (isFile) {
		if (isTarget)
			pStream = MediaFile::Writer::New(path, format.c_str(), ioFile);
		else
			pStream = MediaFile::Reader::New(path, source, format.c_str(), timer, ioFile);
	} else {
		if (!type) // TCP by default excepting if format is RTP where rather UDP by default
			type = String::ICompare(format, "RTP") == 0 ? TYPE_UDP : TYPE_TCP;
		// KEEP this model of double creation to allow a day a new RTPWriter<...>(parameter)
		if (type != TYPE_UDP && (isBind || !address.host())) { // MediaServer
			if (isTarget)
				pStream = MediaServer::Writer::New(MediaServer::Type(type), path, format.c_str(), address, ioSocket, isSecure ? pTLS : nullptr);
			else
				pStream = MediaServer::Reader::New(MediaServer::Type(type), path, source, format.c_str(), address, ioSocket, isSecure ? pTLS : nullptr);
		}
		else if (isTarget)
			pStream = MediaSocket::Writer::New(type, path, format.c_str(), address, ioSocket, isSecure ? pTLS : nullptr);
		else
			pStream = MediaSocket::Reader::New(type, path, source, format.c_str(), address, ioSocket, isSecure ? pTLS : nullptr);
	}

	if (!pStream) {
		ex.set<Ex::Unsupported>(isTarget ? "Target stream " : "Source stream ", TypeToString(type), " format ", format, " not supported");
		return nullptr;
	}
	(string&)pStream->query = move(query);
	return pStream;
}

} // namespace Mona