//
// Copyright (C) 2005 Andras Varga
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this program; if not, see <http://www.gnu.org/licenses/>.
//

#include "inet/networklayer/ipv6/Ipv6ExtHeaderTag_m.h"

#include "inet/networklayer/ipv6/Ipv6Header.h"

namespace inet {

Ipv6ExtensionHeader *Ipv6ExtHeaderTagBase::removeFirstExtensionHeader()
{
    if (extensionHeader_arraysize == 0)
        return nullptr;

    Ipv6ExtensionHeader *ret = dropExtensionHeader(0);
    this->eraseExtensionHeader(0);
    return ret;
}

} // namespace inet

