// update_key.h -- the public half of the key that signs releases.
//
// Empty until the owner has generated one with packaging/make-update-key (see
// packaging/README.md).  While it is empty the update dialog says so and sends you to
// the release page instead of downloading anything.
//
// 64 hex digits: the raw 32-byte Ed25519 public key.

#pragma once

namespace update {

const char kUpdatePublicKey [] = "";

} // namespace update
