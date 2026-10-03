// compat/xgraphics.h
//
// Stand-in for the Xbox SDK header of the same name. xgraphics.h only ever
// shipped with the (long dead) Xbox SDK, and the original Windows build linked
// xgraphics.lib -- but nothing in the tree actually references an XGraphics
// symbol, so an empty header is the whole fix.
//
// The include itself was vestigial: fdx8tex.cpp:33 includes it unconditionally
// and then only uses d3dx8tex.h underneath #if FANG_PLATFORM_WIN.
#pragma once
