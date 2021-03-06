/*
* This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef NETWORK_BUFFER_H
#define NETWORK_BUFFER_H

#include <cstdio>
#include "ProtocolDefinitions.h"

class NetworkBuffer
{
public:
    NetworkBuffer();
    NetworkBuffer(uint32 size);
    NetworkBuffer(uint8* buffer, uint32 size);
    ~NetworkBuffer();

    void Allocate(uint32 size);
    void Reallocate(uint32 new_size);
    void Deallocate();
    void AssignBuffer(uint8* buffer, uint32 size);
    void UnassignBuffer();

    bool Write(const uint8* data, size_t n);
    bool Read(uint8* data, size_t n);
    bool ReadNoConsume(uint8* data, size_t n);

    void Commit(const size_t n);
    void Consume(const size_t n);

    void Prepare();
    bool Crunch();
    void Reset();

    uint8* write_data() const;
    uint8* read_data() const;

    uint32 length() const;
    uint32 space() const;
    uint32 capacity() const;

private:
    bool data_allocated_;
    uint8* data_;

    uint32 write_position_;
    uint32 read_position_;

    uint32 size_;
};

#endif // NETWORK_BUFFER_H
