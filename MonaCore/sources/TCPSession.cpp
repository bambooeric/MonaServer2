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

#include "Mona/TCPSession.h"

using namespace std;


namespace Mona {

TCPSession::TCPSession(Protocol& protocol) : TCPClient(api.ioSocket), onData(TCPClient::onData), _sendingTrack(0), Session(protocol, SocketAddress::Wildcard()) {}

void TCPSession::connect(const shared<Socket>& pSocket) {
	if(!peer.serverAddress.host()) // use TCP client bind to determine server address if need (can have been assigned already by a protocol publicHost, not override in this case)
		peer.setServerAddress(SocketAddress(pSocket->address().host(), 0));
	peer.setAddress(pSocket->peerAddress());
	// don't SetSocketParameters here, wait peer connection to allow it!

	onError = [this](const Exception& ex) { WARN(name(), ", ", ex); };
	onDisconnection = [this](const SocketAddress&) { kill(ERROR_SOCKET); };

	bool success;
	Exception ex;
	AUTO_ERROR(success = TCPClient::connect(ex, pSocket), name());
	if (!success)
		return kill(ERROR_SOCKET);
	onFlush = [this]() { flush(); }; // allow to signal end of congestion, and so was congestion so force flush (HTTPSession/HTTPFileSender uses it for example to continue to read a file)
}

void TCPSession::onParameters(const Parameters& parameters) {
	Session::onParameters(parameters);
	Exception ex;
	AUTO_ERROR(self->processParams(ex, parameters), name(), " socket configuration");
	DEBUG(name(), " socket buffers set to ", self->recvBufferSize(), "B in reception and ", self->sendBufferSize(), "B in sends");
}

void TCPSession::send(const Packet& packet) {
	if (!died)
		return api.threadPool.queue(new TCPClient::Sender(self, packet), _sendingTrack);
	ERROR(name()," tries to send a message after dying");
}

void TCPSession::kill(Int32 error, const char* reason) {
	if(died)
		return;

	// Stop reception
	onData = nullptr;

	Session::kill(error, reason); // onPeerDisconnection, before socket disconnection to allow possible last message

	// unsubscribe events before to avoid to get onDisconnection=>kill again on client.disconnect
	onDisconnection = nullptr;
	onError = nullptr;
	onFlush = nullptr;
	disconnect();
}


} // namespace Mona
