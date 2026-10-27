// Portions Copyright 2026 The Hollycast Authors
#include <gtest/gtest.h>

#include "hw/pvr/ta_ctx.h"
#include "rend/sorter.h"

namespace {

TEST(SorterTest, SortedFlatTrianglesRotateIndicesWithoutMutatingSharedVertices)
{
	rend_context ctx;
	ctx.verts.resize(4);
	for (u32 i = 0; i < ctx.verts.size(); ++i)
		ctx.verts[i].col[0] = i + 1;
	ctx.idx = { 0, 1, 2, 2, 1, 3 };
	PolyParam poly {};
	poly.pcw.Gouraud = false;
	ctx.global_param_tr.push_back(poly);
	ctx.sortedTriangles.push_back({ 0, 0, 6 });

	setFirstProvokingVertex(ctx);

	EXPECT_EQ(ctx.idx, (std::vector<u32> { 2, 0, 1, 3, 2, 1 }));
	EXPECT_EQ(ctx.verts[0].col[0], 1);
	EXPECT_EQ(ctx.verts[1].col[0], 2);
	EXPECT_EQ(ctx.verts[2].col[0], 3);
	EXPECT_EQ(ctx.verts[3].col[0], 4);
}

TEST(SorterTest, SortedGouraudTrianglesAreUnchanged)
{
	rend_context ctx;
	ctx.idx = { 0, 1, 2 };
	PolyParam poly {};
	poly.pcw.Gouraud = true;
	ctx.global_param_tr.push_back(poly);
	ctx.sortedTriangles.push_back({ 0, 0, 3 });

	setFirstProvokingVertex(ctx);

	EXPECT_EQ(ctx.idx, (std::vector<u32> { 0, 1, 2 }));
}

} // namespace
