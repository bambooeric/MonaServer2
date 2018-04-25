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

#include "Mona/WS/WSSession.h"
#include "Mona/StringReader.h"
#include "Mona/JSONReader.h"

using namespace std;


namespace Mona {

WSSession::WSSession(Protocol& protocol, TCPSession& session, shared<WSDecoder> pDecoder) : Session(protocol, session), writer(session), _pSubscription(NULL), _pPublication(NULL),
	_onRequest([this](WS::Request& request) {
		Exception ex;

		switch (request.type) {
			case WS::TYPE_BINARY: {
				processMessage(ex, request, true);
				break;
			}
			case WS::TYPE_TEXT: {
				processMessage(ex, request);
				break;
			}
			case WS::TYPE_CLOSE: {
				BinaryReader reader(request.data(), request.size());
				UInt16 code(reader.read16());
				if (reader.available()) // if message, display a log, otherwise not necessary
					ERROR(name(), " close, ", string(STR reader.current(), reader.available()));
				switch (code) {
					case 0: // no error code
					case WS::CODE_NORMAL_CLOSE:
						// normal error
						kill();
						break;
					case WS::CODE_ENDPOINT_GOING_AWAY:
						// client is dying
						kill(ERROR_SOCKET);
						break;
					case WS::CODE_POLICY_VIOLATION:
						// no permission
						kill(ERROR_REJECTED);
						break;
					case WS::CODE_PROTOCOL_ERROR:
					case WS::CODE_PAYLOAD_NOT_ACCEPTABLE:
					case WS::CODE_MALFORMED_PAYLOAD:
					case WS::CODE_PAYLOAD_TOO_BIG:
						// protocol error
						kill(ERROR_PROTOCOL);
						break;
					case WS::CODE_EXTENSION_REQUIRED:
						// Unsupported
						kill(ERROR_UNSUPPORTED);
						break;
					default:
						// unexpected
						kill(ERROR_UNEXPECTED);
						break;
				}
				return;
			}
			case WS::TYPE_PING:
				writer.writePong(request);
				writer.flush();
				break;
			case WS::TYPE_PONG: {
				UInt32 elapsed0(BinaryReader(request.data(), request.size()).read32());
				UInt32 elapsed1 = UInt32(peer.connection.elapsed());
				if (elapsed1>elapsed0)
					peer.setPing(elapsed1 - elapsed0);
				return;
			}
			default:
				ERROR(ex.set<Ex::Protocol>(name(), String::Format<UInt8>(" request type %#x unknown", request.type)));
				break;
		}

		// Close the session on exception beause nothing is expected in WebSocket to send an error excepting in "close" message (has a code and a string)
		if (ex)
			return kill(ToError(ex), ex.c_str());

		if (request.flush)
			flush();
	}) {
	pDecoder->onRequest = _onRequest;
}

void WSSession::kill(Int32 error, const char* reason) {
	if (died)
		return;

	// Stop reception
	_onRequest = nullptr;

	// unpublish and unsubscribe
	unpublish();
	unsubscribe();

	// onDisconnection after "unpublish or unsubscribe", but BEFORE _writers.clear() because call onDisconnection and writers can be used here
	Session::kill(error, reason);

	// close writer (flush) and release socket resources
	writer.close(error, reason);
}

void WSSession::subscribe(Exception& ex, string& stream, WSWriter& writer) {
	if(!_pSubscription)
		_pSubscription = new Subscription(writer);
	if (api.subscribe(ex, stream, peer, *_pSubscription))
		return;
	delete _pSubscription;
	_pSubscription = NULL;
}
void WSSession::unsubscribe(){
	if (!_pSubscription)
		return;
	api.unsubscribe(peer,*_pSubscription);
	delete _pSubscription;
	_pSubscription = NULL;
}

void WSSession::publish(Exception& ex, string& stream) {
	unpublish();
	_media = Media::TYPE_NONE;
	_track = 0; // default for data!
	_pPublication=api.publish(ex, peer, stream);
}
void WSSession::unpublish() {
	if (!_pPublication)
		return;
	api.unpublish(*_pPublication, peer);
	_pPublication=NULL;
}

void WSSession::processMessage(Exception& ex, const Packet& message, bool isBinary) {

	unique_ptr<DataReader> pReader(isBinary ? new StringReader(message.data(), message.size()) : Media::Data::NewReader(Media::Data::TYPE_JSON, message)); // Use NewReader to check JSON validity
	string name;
	bool isJSON(!isBinary && pReader);
	if (isJSON && pReader->readString(name) && name[0]=='@') {
		if (name == "@publish") {
			if (!pReader->readString(name)) {
				ERROR(ex.set<Ex::Protocol>("@publish method takes a stream name in first parameter"));
				return;
			}
			return publish(ex, name);
		}

		if (name == "@subscribe") {
			if (pReader->readString(name))
				return subscribe(ex, name, writer);
			ERROR(ex.set<Ex::Protocol>("@subscribe method takes a stream name in first parameter"));
			return;
		}

		if (name == "@unpublish")
			return unpublish();


		if (name == "@unsubscribe")
			return unsubscribe();
	
	} else if (_pPublication) {
		if (!isBinary)
			return _pPublication->writeData(isJSON ? Media::Data::TYPE_JSON : Media::Data::TYPE_UNKNOWN, message, _track);

		// Binary => audio or video or data

		Packet content(message.buffer(), message.data(), message.size());

		if(!_media) {
			BinaryReader header(message.data(), message.size());

			_media = Media::Unpack(header, _audio, _video, _data, _track);
			if (!_media) {
				ERROR("Malformed media header size");
				return;
			}
			if (!(content+=header.position()))
				return; // wait next binary!
		}
		switch (_media) {
			case Media::TYPE_AUDIO:
				_pPublication->writeAudio(_audio, content, _track);
				break;
			case Media::TYPE_VIDEO:
				_pPublication->writeVideo(_video, content, _track);
				break;
			default:
				_pPublication->writeData(_data, content, _track);
		}
		// reset header info because we have get the related content
		_track = 0;
		_media = Media::TYPE_NONE;
		return;
	}

	if (!pReader)
		pReader.reset(new StringReader(message.data(), message.size()));
	if (!peer.onInvocation(ex, name, *pReader, isBinary ? WS::TYPE_BINARY : (!isJSON  ? WS::TYPE_TEXT : 0)) && !ex)
		ERROR(ex.set<Ex::Application>("Method client ", name, " not found in application ", peer.path));
}


bool WSSession::manage() {
	if (!Session::manage())
		return false;
	if (peer && peer.pingTime.isElapsed(timeout/2)) {
		writer.writePing();
		peer.pingTime.update();
	}
	// check subscription
	if (!_pSubscription)
		return true;
	switch (_pSubscription->ejected()) {
		case Subscription::EJECTED_BANDWITDH:
			writer.writeInvocation("@unsubscribe").writeString(EXPAND("Insufficient bandwidth"));
			break;
		case Subscription::EJECTED_ERROR:
			writer.writeInvocation("@unsubscribe").writeString(EXPAND("Unknown error"));
			break;
		default: return true;// no ejected!
	}
	unsubscribe();
	return true;
}

void WSSession::flush() {
	// flush publication
	if (_pPublication)
		_pPublication->flush(peer.ping());
	// flush writer
	writer.flush();
}

} // namespace Mona
