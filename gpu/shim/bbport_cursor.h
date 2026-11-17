// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace BbCursor {
// Hides the mouse cursor until the mouse next moves (macOS; works with a still mouse).
void HideUntilMouseMoves();
// After the window went full screen or moved to another display during the first seconds:
// takes the focus back if macOS gave it to another app on that display (Spaces switch).
void ActivateAfterWindowMove(const char* why);
} // namespace BbCursor
