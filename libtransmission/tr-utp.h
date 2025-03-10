// This file Copyright © 2010 Juliusz Chroboczek.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstddef> // size_t

bool tr_utpPacket(unsigned char const* buf, size_t buflen, struct sockaddr const* from, socklen_t fromlen, tr_session* ss);

void tr_utpClose(tr_session*);

void tr_utpSendTo(void* closure, unsigned char const* buf, size_t buflen, struct sockaddr const* to, socklen_t tolen);
