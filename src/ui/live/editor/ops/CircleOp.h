// license:GPLv3+

#pragma once

#include "CircleMath.h"
#include "CurveEditOp.h"

namespace VPX::EditorUI
{

// Point edit mode > Make Circle (Shift+Alt+C): replaces the whole outline of the edited wall by a regular polygon (12 faces by default) fitted to
// the selected points (at least 3): center at their average position, radius at their mean distance to it, first point towards the first selected
// point, same winding as the outline. New round walls are added with Add Part > Round Wall (CircleWallOp). Refused on open curves
// (ramps) and if a point of the outline is locked. The generated points are plain (not smooth) drag points that copy the other properties of the
// first point of the outline, slingshot only if all segments were. See CircleMath for the geometry.
class CircleOp final : public CurveEditOp::Tool
{
public:
   const char *GetName() const override { return "Make Circle"; }
   int GetShortcut() const override;
   bool Start(const EditorUIPart &part, const vector<CurveEditOp::PointState> &points, const vector<int> &sel, bool closed, string &error) override;
   bool Generate(const EditorUIPart &part, const vector<CurveEditOp::PointState> &points, const vector<int> &sel, bool closed, vector<CurveEditOp::PointState> &out,
      vector<int> &outSel) override;
   bool RenderParameters(const EditorUIPart &part, const std::function<void(const char *)> &row) override;

private:
   int m_faces = 12;
   CircleMath::Fit m_fit; // Fitted to the selected points when the tool is applied, then edited with Adjust Last Operation
};

}
