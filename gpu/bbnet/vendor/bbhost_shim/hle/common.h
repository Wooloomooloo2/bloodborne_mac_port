// SPDX-License-Identifier: GPL-3.0-or-later
// Adapter for the vendored bbhost NpSignaling: it registers its imports by name; the glue
// (thd/thd_bbhost_glue.cpp) keeps the table and binds them to the game's NIDs.
#pragma once
void register_hle_fn(const char* name, void* fn);
