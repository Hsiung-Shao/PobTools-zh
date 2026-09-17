// PobToolsTextureAtlas: decodes a POB texture (DDS arrays, BC1/BC7, zstd) into
// cached PNG pages for the new UI. Headless only; see texture_atlas.cpp.
#pragma once

struct lua_State;

namespace TextureAtlas {

void Register(lua_State* L);

} // namespace TextureAtlas
