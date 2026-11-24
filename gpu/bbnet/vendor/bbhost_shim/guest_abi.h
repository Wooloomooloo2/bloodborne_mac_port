// SPDX-License-Identifier: GPL-3.0-or-later
// Adapter for the vendored bbhost NpSignaling (../bbhost/np_signaling.cpp): the calling convention
// of the game's imports. The game's code is x86-64 System V; so is the module (Rosetta), so this
// is the same attribute shadPS4's PS4_SYSV_ABI expands to.
#pragma once
#define GUEST_ABI __attribute__((sysv_abi))
