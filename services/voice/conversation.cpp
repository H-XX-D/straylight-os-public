// services/voice/conversation.cpp
// Conversation state management — the header-only implementation covers most
// logic.  This file provides the non-inline helpers for save/load validation
// and conversation summarization.

#include "conversation.h"

#include <filesystem>

namespace straylight::voice {

// Currently all Conversation logic lives in the header (inline).
// This translation unit exists so the linker has something to reference
// and for future expansion (e.g., conversation summarization via LLM,
// serialization to SQLite, etc.).

} // namespace straylight::voice
