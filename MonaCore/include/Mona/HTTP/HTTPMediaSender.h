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

#pragma once

#include "Mona/Mona.h"
#include "Mona/HTTP/HTTPSender.h"


namespace Mona {

struct HTTPMediaSender : HTTPSender, virtual Object {
	HTTPMediaSender(const shared<const HTTP::Header>& pRequest,
		shared<MediaWriter>& pWriter,
		Media::Base* pMedia=NULL);

	bool hasHeader() const { return _first; }

private:
	void run();

	bool _first;
	shared<MediaWriter> _pWriter;
	unique<Media::Base>	_pMedia;
};


} // namespace Mona
