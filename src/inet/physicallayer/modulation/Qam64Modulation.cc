//
// Copyright (C) 2014 OpenSim Ltd.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//

#include "inet/physicallayer/modulation/Qam64Modulation.h"

namespace inet {

namespace physicallayer {

const double k = 1 / sqrt(42);

const std::vector<ApskSymbol> Qam64Modulation::constellation = {
    k * ApskSymbol(-7, -7), k * ApskSymbol(7, -7), k * ApskSymbol(-1, -7), k * ApskSymbol(1, -7), k * ApskSymbol(-5, -7),
    k * ApskSymbol(5, -7), k * ApskSymbol(-3, -7), k * ApskSymbol(3, -7), k * ApskSymbol(-7, 7), k * ApskSymbol(7, 7),
    k * ApskSymbol(-1, 7), k * ApskSymbol(1, 7), k * ApskSymbol(-5, 7), k * ApskSymbol(5, 7), k * ApskSymbol(-3, 7),
    k * ApskSymbol(3, 7), k * ApskSymbol(-7, -1), k * ApskSymbol(7, -1), k * ApskSymbol(-1, -1), k * ApskSymbol(1, -1),
    k * ApskSymbol(-5, -1), k * ApskSymbol(5, -1), k * ApskSymbol(-3, -1), k * ApskSymbol(3, -1), k * ApskSymbol(-7, 1),
    k * ApskSymbol(7, 1), k * ApskSymbol(-1, 1), k * ApskSymbol(1, 1), k * ApskSymbol(-5, 1), k * ApskSymbol(5, 1),
    k * ApskSymbol(-3, 1), k * ApskSymbol(3, 1), k * ApskSymbol(-7, -5), k * ApskSymbol(7, -5), k * ApskSymbol(-1, -5),
    k * ApskSymbol(1, -5), k * ApskSymbol(-5, -5), k * ApskSymbol(5, -5), k * ApskSymbol(-3, -5), k * ApskSymbol(3, -5),
    k * ApskSymbol(-7, 5), k * ApskSymbol(7, 5), k * ApskSymbol(-1, 5), k * ApskSymbol(1, 5), k * ApskSymbol(-5, 5),
    k * ApskSymbol(5, 5), k * ApskSymbol(-3, 5), k * ApskSymbol(3, 5), k * ApskSymbol(-7, -3), k * ApskSymbol(7, -3),
    k * ApskSymbol(-1, -3), k * ApskSymbol(1, -3), k * ApskSymbol(-5, -3), k * ApskSymbol(5, -3), k * ApskSymbol(-3, -3),
    k * ApskSymbol(3, -3), k * ApskSymbol(-7, 3), k * ApskSymbol(7, 3), k * ApskSymbol(-1, 3), k * ApskSymbol(1, 3),
    k * ApskSymbol(-5, 3), k * ApskSymbol(5, 3), k * ApskSymbol(-3, 3), k * ApskSymbol(3, 3)
};

const Qam64Modulation Qam64Modulation::singleton;

Qam64Modulation::Qam64Modulation() : MqamModulationBase(&constellation)
{
}

} // namespace physicallayer

} // namespace inet

