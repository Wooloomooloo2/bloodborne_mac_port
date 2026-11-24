// SPDX-License-Identifier: GPL-3.0-or-later
// Adapter for the vendored bbhost NpSignaling: its log lines go to the backend's log (the glue).
#pragma once
void bbhost_np_log(const char* format, ...) __attribute__((format(printf, 1, 2)));
#define host_log bbhost_np_log
