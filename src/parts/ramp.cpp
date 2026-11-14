// license:GPLv3+

#include "core/stdafx.h"
#include "ramp.h"

#include "core/VPApp.h"
#include "parts/Collection.h"
#include "physics/collideex.h"
#include "physics/PhysicsEngine.h"
#include "renderer/RenderDevice.h"
#include "renderer/Renderer.h"
#include "renderer/Shader.h"
#include "renderer/Texture.h"
#include "renderer/trace.h"
#include "utils/objloader.h"


Ramp::~Ramp()
{
   assert(m_renderer == nullptr);
   delete[] m_rgheightInit;
}

Ramp *Ramp::CopyForPlay() const
{
   STANDARD_EDITABLE_WITH_DRAGPOINT_COPY_FOR_PLAY_IMPL(Ramp, m_curve)
   return dst;
}

HRESULT Ramp::Init(const float x, const float y, const bool fromMouseClick, const bool forPlay)
{
   SetDefaults(fromMouseClick);
   m_d.m_visible = true;

   const float length = 0.5f * g_settingsService.GetAppSettings().GetDefaultPropsRamp_Length();

   auto pdp = std::make_unique<DragPoint>(&m_curve, x, y + length, 0.f, true);
   pdp->SetCalcHeight(m_d.m_heightbottom);
   m_curve.PushPoint(std::move(pdp));

   pdp = std::make_unique<DragPoint>(&m_curve, x, y - length, 0.f, true);
   pdp->SetCalcHeight(m_d.m_heighttop);
   m_curve.PushPoint(std::move(pdp));

   return S_OK;
}

#define LinkProp(field, prop) field = fromMouseClick ? g_settingsService.GetAppSettings().GetDefaultPropsRamp_##prop() : Settings::GetDefaultPropsRamp_##prop##_Default()
void Ramp::SetDefaults(const bool fromMouseClick)
{
   LinkProp(m_d.m_heightbottom, HeightBottom);
   LinkProp(m_d.m_heighttop, HeightTop);
   LinkProp(m_d.m_widthbottom, WidthBottom);
   LinkProp(m_d.m_widthtop, WidthTop);
   LinkProp(m_d.m_type, RampType);
   LinkProp(m_d.m_szImage, Image);
   LinkProp(m_d.m_imagealignment, ImageMode);
   LinkProp(m_d.m_imageWalls, ImageWalls);
   LinkProp(m_d.m_leftwallheight, LeftWallHeight);
   LinkProp(m_d.m_rightwallheight, RightWallHeight);
   LinkProp(m_d.m_hitEvent, HitEvent);
   LinkProp(m_d.m_threshold, HitThreshold);
   LinkProp(m_d.m_collidable, Collidable);
   LinkProp(m_d.m_visible, Visible);
   LinkProp(m_d.m_wireDiameter, WireDiameter);
   LinkProp(m_d.m_wireDistanceX, WireDistanceX);
   LinkProp(m_d.m_wireDistanceY, WireDistanceY);
   LinkProp(m_d.m_reflectionEnabled, ReflectionEnabled);
   LinkProp(m_timerEnabled, TimerEnabled);
   LinkProp(m_timerInterval, TimerInterval);
   SetDefaultPhysics(fromMouseClick);
}

void Ramp::SetDefaultPhysics(const bool fromMouseClick)
{
   LinkProp(m_d.m_elasticity, Elasticity);
   LinkProp(m_d.m_friction, Friction);
   LinkProp(m_d.m_scatter, Scatter);
}
#undef LinkProp

void Ramp::WriteRegDefaults()
{
#define LinkProp(field, prop) g_settingsService.GetAppSettings().SetDefaultPropsRamp_##prop(field, false)
   LinkProp(m_d.m_heightbottom, HeightBottom);
   LinkProp(m_d.m_heighttop, HeightTop);
   LinkProp(m_d.m_widthbottom, WidthBottom);
   LinkProp(m_d.m_widthtop, WidthTop);
   LinkProp(m_d.m_type, RampType);
   LinkProp(m_d.m_szImage, Image);
   LinkProp(m_d.m_imagealignment, ImageMode);
   LinkProp(m_d.m_imageWalls, ImageWalls);
   LinkProp(m_d.m_leftwallheight, LeftWallHeight);
   LinkProp(m_d.m_rightwallheight, RightWallHeight);
   LinkProp(m_d.m_hitEvent, HitEvent);
   LinkProp(m_d.m_threshold, HitThreshold);
   LinkProp(m_d.m_collidable, Collidable);
   LinkProp(m_d.m_visible, Visible);
   LinkProp(m_d.m_wireDiameter, WireDiameter);
   LinkProp(m_d.m_wireDistanceX, WireDistanceX);
   LinkProp(m_d.m_wireDistanceY, WireDistanceY);
   LinkProp(m_d.m_elasticity, Elasticity);
   LinkProp(m_d.m_friction, Friction);
   LinkProp(m_d.m_scatter, Scatter);
   LinkProp(m_d.m_reflectionEnabled, ReflectionEnabled);
   LinkProp(m_timerEnabled, TimerEnabled);
   LinkProp(m_timerInterval, TimerInterval);
#undef LinkProp
}

void Ramp::GetBoundingVertices(vector<Vertex3Ds> &bounds, vector<Vertex3Ds> *const legacy_bounds)
{
   if (legacy_bounds == nullptr && !m_d.m_visible)
      return;

   //!! meh, this is delivering something loosely related to the bounding vertices, but its only used in the cam fitting code so far, so keep for legacy reasons
   float *rgheight1;
   int cvertex;
   const Vertex2D * const rgvLocal = GetRampVertex(cvertex, &rgheight1, nullptr, nullptr, nullptr, HIT_SHAPE_DETAIL_LEVEL, true);

   //if(m_d.m_visible) bounds.reserve(bounds.size() + cvertex * 2);
   Vertex3Ds bbox_min(FLT_MAX, FLT_MAX, FLT_MAX);
   Vertex3Ds bbox_max(-FLT_MAX, -FLT_MAX, -FLT_MAX);
   for (int i = 0; i < cvertex; i++)
   {
	  {
	  const Vertex3Ds pv(rgvLocal[i].x,rgvLocal[i].y,rgheight1[i] + (float)(2.0*PHYS_SKIN)); // leave room for ball //!! use ballsize
	  //if(m_d.m_visible) bounds.push_back(pv);
	  bbox_min.x = min(bbox_min.x, pv.x);
	  bbox_min.y = min(bbox_min.y, pv.y);
	  bbox_min.z = min(bbox_min.z, pv.z);
	  bbox_max.x = max(bbox_max.x, pv.x);
	  bbox_max.y = max(bbox_max.y, pv.y);
	  bbox_max.z = max(bbox_max.z, pv.z);
	  }

	  const Vertex3Ds pv(rgvLocal[cvertex * 2 - i - 1].x,rgvLocal[cvertex * 2 - i - 1].y,rgheight1[i] + (float)(2.0*PHYS_SKIN)); // leave room for ball //!! use ballsize
	  //if(m_d.m_visible) bounds.push_back(pv);
	  bbox_min.x = min(bbox_min.x, pv.x);
	  bbox_min.y = min(bbox_min.y, pv.y);
	  bbox_min.z = min(bbox_min.z, pv.z);
	  bbox_max.x = max(bbox_max.x, pv.x);
	  bbox_max.y = max(bbox_max.y, pv.y);
	  bbox_max.z = max(bbox_max.z, pv.z);
   }

   delete[] rgvLocal;
   delete[] rgheight1;

   // returns all 8 corners as this will be used for further transformations later-on
   for (int i = 0; i < 8; i++)
   {
	   const Vertex3Ds pv(
		   (i & 1) ? bbox_min.x : bbox_max.x,
		   (i & 2) ? bbox_min.y : bbox_max.y,
		   (i & 4) ? bbox_min.z : bbox_max.z);

	   if (m_d.m_visible)
		   bounds.push_back(pv);
	   if (legacy_bounds)
		   legacy_bounds->push_back(pv);
   }
}

void Ramp::AssignHeightToControlPoint(const RenderVertex3D &v, const float height) const
{
   for (size_t i = 0; i < m_curve.GetPoints().size(); i++)
   {
      if (m_curve.GetPoints()[i]->GetX() == v.x && m_curve.GetPoints()[i]->GetY() == v.y)
         m_curve.GetPoints()[i]->SetCalcHeight(height);
   }
}

// Ported at: VisualPinball.Engine/VPT/Ramp/RampMeshGenerator.cs

/*
 * Compute the vertices and additional information for the ramp shape.
 *
 * Output:
 *  pcvertex     - number of vertices for the central curve
 *  return value - size 2*cvertex, vertices forming the 2D outline of the ramp
 *                 order: first forward along right side of ramp, then backward along the left side
 *  ppheight     - size cvertex, height of the ramp at the i-th vertex
 *  ppfCross     - size cvertex, true if i-th vertex corresponds to a control point
 *  ppratio      - how far along the ramp length the i-th vertex is, 1=start=bottom, 0=end=top (??)
 */
Vertex2D *Ramp::GetRampVertex(int &pcvertex, float **const ppheight, bool **const ppfCross, float **const ppratio, Vertex2D **const pMiddlePoints, const float _accuracy,
   const bool inc_width, bool **const ppWall) const
{
   if (ppWall)
      *ppWall = nullptr;
   if (HasActiveSections())
   {
      SectionRows rows;
      if (BuildSectionedRows(rows, nullptr))
      {
         const int n = (int)rows.middle.size();
         pcvertex = n;
         Vertex2D *const rgv = new Vertex2D[n * 2];
         for (int i = 0; i < n; i++)
         {
            rgv[i] = rows.right[i];
            rgv[n * 2 - i - 1] = rows.left[i];
         }
         if (ppheight)
         {
            *ppheight = new float[n];
            std::copy(rows.height.begin(), rows.height.end(), *ppheight);
         }
         if (ppfCross)
         {
            *ppfCross = new bool[n];
            std::copy(rows.cross.begin(), rows.cross.end(), *ppfCross);
         }
         if (ppratio)
         {
            *ppratio = new float[n];
            std::copy(rows.ratio.begin(), rows.ratio.end(), *ppratio);
         }
         if (pMiddlePoints)
         {
            *pMiddlePoints = new Vertex2D[n];
            std::copy(rows.middle.begin(), rows.middle.end(), *pMiddlePoints);
         }
         if (ppWall)
         {
            *ppWall = new bool[n];
            std::copy(rows.wall.begin(), rows.wall.end(), *ppWall);
         }
         return rgv;
      }
   }

   vector<RenderVertex3D> vvertex;
   GetCentralCurve(vvertex, _accuracy);
   // vvertex are the 2D vertices forming the central curve of the ramp as seen from above

   const int cvertex = (int)vvertex.size();
   pcvertex = cvertex;
   Vertex2D * const rgvLocal = new Vertex2D[cvertex * 2];

   if (ppheight)
   {
      *ppheight = new float[cvertex];
   }
   if (ppfCross)
   {
      *ppfCross = new bool[cvertex];
   }
   if (ppratio)
   {
      *ppratio = new float[cvertex];
   }
   if (pMiddlePoints)
   {
      *pMiddlePoints = new Vertex2D[cvertex];
   }

   // Compute an approximation to the length of the central curve
   // by adding up the lengths of the line segments.
   float totallength = 0;
   const float bottomHeight = m_d.m_heightbottom;
   const float topHeight = m_d.m_heighttop;

   for (int i = 0; i < (cvertex - 1); i++)
   {
      const RenderVertex3D & v1 = vvertex[i];
      const RenderVertex3D & v2 = vvertex[i + 1];

      const float dx = v1.x - v2.x;
      const float dy = v1.y - v2.y;
      const float length = sqrtf(dx*dx + dy*dy);

      totallength += length;
   }

   float currentlength = 0;
   for (int i = 0; i < cvertex; i++)
   {
      // clamp next and prev as ramps do not loop
      const RenderVertex3D & vprev = vvertex[(i > 0) ? i - 1 : i];
      const RenderVertex3D & vnext = vvertex[(i < (cvertex - 1)) ? i + 1 : i];
      const RenderVertex3D & vmiddle = vvertex[i];

      if (ppfCross)
         (*ppfCross)[i] = vmiddle.controlPoint;

      Vertex2D vnormal;
      {
         // Get normal at this point
         // Notice that these values equal the ones in the line
         // equation and could probably be substituted by them.
         Vertex2D v1normal(vprev.y - vmiddle.y, vmiddle.x - vprev.x);   // vector vmiddle-vprev rotated RIGHT
         Vertex2D v2normal(vmiddle.y - vnext.y, vnext.x - vmiddle.x);   // vector vnext-vmiddle rotated RIGHT

         // special handling for beginning and end of the ramp, as ramps do not loop
         if (i == (cvertex - 1))
         {
            v1normal.Normalize();
            vnormal = v1normal;
         }
         else if (i == 0)
         {
            v2normal.Normalize();
            vnormal = v2normal;
         }
         else
         {
            v1normal.Normalize();
            v2normal.Normalize();

            if (fabsf(v1normal.x - v2normal.x) < 0.0001f && fabsf(v1normal.y - v2normal.y) < 0.0001f)
            {
               // Two parallel segments
               vnormal = v1normal;
            }
            else
            {
               // Find intersection of the two edges meeting this points, but
               // shift those lines outwards along their normals

               // First line
               const float A = vprev.y - vmiddle.y;
               const float B = vmiddle.x - vprev.x;

               // Shift line along the normal
               const float C = A*(v1normal.x - vprev.x) + B*(v1normal.y - vprev.y);

               // Second line
               const float D = vnext.y - vmiddle.y;
               const float E = vmiddle.x - vnext.x;

               // Shift line along the normal
               const float F = D*(v2normal.x - vnext.x) + E*(v2normal.y - vnext.y);

               const float det = A*E - B*D;
               const float inv_det = (det != 0.0f) ? 1.0f / det : 0.0f;

               const float intersectx = (B*F - E*C)*inv_det;
               const float intersecty = (C*D - A*F)*inv_det;

               vnormal.x = vmiddle.x - intersectx;
               vnormal.y = vmiddle.y - intersecty;
            }
         }
      }

      // Update current length along the ramp.
      {
         const float dx = vprev.x - vmiddle.x;
         const float dy = vprev.y - vmiddle.y;
         const float length = sqrtf(dx*dx + dy*dy);

         currentlength += length;
      }

      const float percentage = (totallength != 0.f) ? currentlength / totallength : 0.f;
      float widthcur = percentage * (m_d.m_widthtop - m_d.m_widthbottom) + m_d.m_widthbottom;
      if (ppheight)
      {
         (*ppheight)[i] = vmiddle.z + percentage * (topHeight - bottomHeight) + bottomHeight;
      }

      AssignHeightToControlPoint(vvertex[i], vmiddle.z + percentage * (topHeight - bottomHeight) + bottomHeight);
      if (ppratio)
      {
         (*ppratio)[i] = 1.0f - percentage;
      }
      // only change the width if we want to create vertices for rendering or for the editor
      // the collision engine uses flat type ramps
      if (IsHabitrail() && m_d.m_type != RampType1Wire)
      {
         widthcur = m_d.m_wireDistanceX;
         if (inc_width)
            widthcur += 20.0f;
      }
      else if (m_d.m_type == RampType1Wire)
      {
         widthcur = m_d.m_wireDiameter;
      }

      if (pMiddlePoints)
      {
         (*pMiddlePoints)[i] = Vertex2D(vmiddle.x, vmiddle.y) + vnormal;
      }
      rgvLocal[i                  ] = Vertex2D(vmiddle.x, vmiddle.y) + (widthcur*0.5f) * vnormal;
      rgvLocal[cvertex * 2 - i - 1] = Vertex2D(vmiddle.x, vmiddle.y) - (widthcur*0.5f) * vnormal;
   }

   return rgvLocal;
}

#pragma region Sections

namespace
{
   float Cross2(const Vertex2D &a, const Vertex2D &b) { return a.x * b.y - a.y * b.x; }

   float PointSegmentDistance(const Vertex2D &p, const Vertex2D &a, const Vertex2D &b)
   {
      const Vertex2D ab = b - a;
      const float len2 = ab.LengthSquared();
      const float t = (len2 > 1e-12f) ? clamp((p - a).Dot(ab) / len2, 0.f, 1.f) : 0.f;
      return (p - (a + ab * t)).Length();
   }

   // Shortest distance between two 2D segments
   float SegmentDistance(const Vertex2D &a0, const Vertex2D &a1, const Vertex2D &b0, const Vertex2D &b1)
   {
      const float d1 = Cross2(a1 - a0, b0 - a0), d2 = Cross2(a1 - a0, b1 - a0), d3 = Cross2(b1 - b0, a0 - b0), d4 = Cross2(b1 - b0, a1 - b0);
      if (((d1 > 0.f) != (d2 > 0.f)) && ((d3 > 0.f) != (d4 > 0.f)))
         return 0.f; // crossing
      return min(min(PointSegmentDistance(a0, b0, b1), PointSegmentDistance(a1, b0, b1)), min(PointSegmentDistance(b0, a0, a1), PointSegmentDistance(b1, a0, a1)));
   }

   // Splits total faces over corners in proportion to their turn, at least one each (largest remainder)
   vector<int> SplitFaces(int total, const vector<float> &weights)
   {
      const int n = (int)weights.size();
      vector<int> out(n, 1);
      total = max(total, n);
      float sum = 0.f;
      for (const float w : weights)
         sum += w;
      if (sum <= 0.f)
         return out;
      vector<std::pair<float, int>> rem;
      int used = 0;
      for (int i = 0; i < n; i++)
      {
         const float exact = (float)total * weights[i] / sum;
         out[i] = max(1, (int)exact);
         used += out[i];
         rem.emplace_back(exact - (float)(int)exact, i);
      }
      std::ranges::sort(rem, [](const auto &l, const auto &r) { return l.first > r.first; });
      for (size_t i = 0; used < total && i < rem.size(); i++, used++)
         out[rem[i].second]++;
      return out;
   }

   // Whether a hairpin corner's floor folds with these face counts: rows step along the arc, each side snapped to its own count, and every
   // quad between consecutive rows must keep the turn's orientation for both of its diagonals (a coarse outside chord can cut inside the
   // inside wall, which inverts the quad). Centre at the origin, arc from angle 0.
   bool ArcFolds(const float radius, const float width, const float sweep, const int outer, const int inner)
   {
      const int steps = max(outer, inner);
      const float sgn = (sweep > 0.f) ? 1.f : -1.f;
      const auto pt = [=](int j, int count, float r)
      {
         const int k = (int)lroundf((float)j * (float)count / (float)steps);
         const float a = sweep * (float)k / (float)count;
         return Vertex2D(r * cosf(a), r * sinf(a));
      };
      const auto area = [](const Vertex2D &a, const Vertex2D &b, const Vertex2D &c) { return Cross2(b - a, c - a); };
      const float ro = radius + 0.5f * width, ri = radius - 0.5f * width;
      for (int j = 0; j < steps; j++)
      {
         const Vertex2D o0 = pt(j, outer, ro), o1 = pt(j + 1, outer, ro), i0 = pt(j, inner, ri), i1 = pt(j + 1, inner, ri);
         for (const float t : { area(o0, o1, i1), area(o0, i1, i0), area(o0, o1, i0), area(o1, i1, i0) })
            if (t * sgn < -1e-3f)
               return true;
      }
      return false;
   }

   // Flap bow shape: 0 at both ends, 1 at the low point f (fraction of the flap length), flat tangent there
   float FlapBowShape(const float u, const float f)
   {
      if (u <= f)
      {
         const float v = 1.f - u / f;
         return 1.f - v * v;
      }
      const float v = (u - f) / (1.f - f);
      return 1.f - v * v;
   }
}

// The sectioned ramp geometry: the plain ramp's rows (same mitred normals, length based width and height) over a path in which
// every hairpin corner is replaced by an arc, then each side of an arc is resampled to its own face count, and the start of the ramp
// is replaced by the entry flap's rows. Render, physics and the editor outline all use these rows (GetRampVertex), sampled at the
// physics detail level whatever the caller asks, so what is drawn is what collides.
bool Ramp::BuildSectionedRows(SectionRows &rows, vector<RampSectionStatus> *status) const
{
   const vector<RampSection> &sections = m_d.m_sections;
   if (status)
      status->assign(sections.size(), RampSectionStatus());
   const auto report = [status](size_t i, const string &msg)
   {
      if (status)
         (*status)[i].m_message = msg;
   };

   vector<RenderVertex3D> vv;
   GetCentralCurve(vv, HIT_SHAPE_DETAIL_LEVEL);
   const int npts = (int)m_curve.GetPoints().size();
   if (vv.size() < 2)
   {
      for (size_t i = 0; i < sections.size(); i++)
         report(i, "The ramp path has fewer than 2 points"s);
      return false;
   }

   // Curve vertex of each control point (open curve: the last one is the final vertex)
   vector<int> cpVert;
   for (int i = 0; i < (int)vv.size() - 1; i++)
      if (vv[i].controlPoint)
         cpVert.push_back(i);
   cpVert.push_back((int)vv.size() - 1);
   const bool mapped = (int)cpVert.size() == npts;
   const auto cpPos = [&vv, &cpVert](int k) { return Vertex2D(vv[cpVert[k]].x, vv[cpVert[k]].y); };

   const float wMax = max(m_d.m_widthbottom, m_d.m_widthtop);
   const float minRadius = wMax * 0.5f + 1.f;

   // ---- Hairpins: plan each one (arcs fitted into the corners of its range)
   struct Corner
   {
      Vertex2D t1, t2, c;
      float a0, sweep;
      int outer, inner;
   };
   struct Plan
   {
      size_t section;
      int first, last;
      float radius;
      vector<Corner> corners;
   };
   vector<Plan> plans;
   {
      vector<size_t> order;
      for (size_t i = 0; i < sections.size(); i++)
         if (sections[i].m_kind == RampSection::Hairpin)
            order.push_back(i);
      std::ranges::stable_sort(order, [&sections](size_t l, size_t r) { return sections[l].m_first < sections[r].m_first; });
      int prevLast = -2;
      for (const size_t si : order)
      {
         const RampSection &s = sections[si];
         if (status)
            (*status)[si].m_minRadius = minRadius;
         if (!mapped)
         {
            report(si, "Two ramp points coincide: separate them"s);
            continue;
         }
         if (s.m_first < 1 || s.m_last > npts - 2 || s.m_first > s.m_last)
         {
            report(si, "A hairpin needs interior points of the path (not its ends)"s);
            continue;
         }
         if (s.m_first <= prevLast + 1)
         {
            report(si, "Touches another hairpin: leave a point between them"s);
            continue;
         }
         if (s.m_radius < minRadius)
         {
            report(si, "Radius is under half the ramp width + 1: the inside would fold"s);
            continue;
         }
         Plan plan { si, s.m_first, s.m_last, s.m_radius, {} };
         vector<float> turns, tans;
         vector<Vertex2D> d1s, d2s;
         string fail;
         float totalTurn = 0.f;
         for (int k = s.m_first; k <= s.m_last && fail.empty(); k++)
         {
            Vertex2D d1 = cpPos(k) - cpPos(k - 1), d2 = cpPos(k + 1) - cpPos(k);
            const float len1 = d1.Length(), len2 = d2.Length();
            if (len1 < 0.01f || len2 < 0.01f)
            {
               fail = "Point " + std::to_string(k) + " sits on its neighbour";
               break;
            }
            d1 = d1 * (1.f / len1);
            d2 = d2 * (1.f / len2);
            const float turn = atan2f(Cross2(d1, d2), d1.Dot(d2));
            if (fabsf(turn) < ANGTORAD(1.f))
               fail = "The path does not turn at point " + std::to_string(k);
            else if (fabsf(turn) > ANGTORAD(178.f))
               fail = "The path doubles back at point " + std::to_string(k) + ": spread the turn over two points";
            else
            {
               const float room = min(len1 * ((k - 1 >= s.m_first) ? 0.5f : 1.f), len2 * ((k + 1 <= s.m_last) ? 0.5f : 1.f));
               const float tn = tanf(fabsf(turn) * 0.5f);
               plan.radius = min(plan.radius, room / tn);
               turns.push_back(turn);
               tans.push_back(tn);
               d1s.push_back(d1);
               d2s.push_back(d2);
               totalTurn += turn;
            }
         }
         if (!fail.empty())
         {
            report(si, fail);
            continue;
         }
         if (status)
            (*status)[si].m_radius = plan.radius;
         if (plan.radius < minRadius)
         {
            char buf[256];
            snprintf(buf, sizeof(buf), "Only %.0f VPU of radius fits between the neighbouring points (%.0f needed): move them further away", plan.radius, minRadius);
            report(si, buf);
            continue;
         }
         vector<float> weights;
         for (const float t : turns)
            weights.push_back(fabsf(t));
         vector<int> outer = SplitFaces(clamp(s.m_outerFaces, 1, 256), weights);
         const vector<int> inner = SplitFaces(clamp(s.m_innerFaces, 1, 256), weights);
         // Raise each corner's outside count until its floor does not fold (checked at both ramp widths: the narrower one is the worse)
         int outerTotal = 0;
         bool folds = false;
         for (size_t c = 0; c < turns.size(); c++)
         {
            while (outer[c] < 256 && (ArcFolds(plan.radius, m_d.m_widthbottom, turns[c], outer[c], inner[c]) || ArcFolds(plan.radius, m_d.m_widthtop, turns[c], outer[c], inner[c])))
               outer[c]++;
            folds |= ArcFolds(plan.radius, m_d.m_widthbottom, turns[c], outer[c], inner[c]) || ArcFolds(plan.radius, m_d.m_widthtop, turns[c], outer[c], inner[c]);
            outerTotal += outer[c];
         }
         if (status)
            (*status)[si].m_outerFaces = outerTotal;
         if (folds)
         {
            report(si, "The floor would fold with these face counts: give the inside more faces"s);
            continue;
         }
         if (outerTotal > max(s.m_outerFaces, (int)turns.size()))
            report(si, "Outside faces raised to " + std::to_string(outerTotal) + " so the floor does not fold over the inside wall");
         for (size_t c = 0; c < turns.size(); c++)
         {
            const Vertex2D p = cpPos(s.m_first + (int)c);
            const float setback = plan.radius * tans[c];
            Corner corner;
            corner.t1 = p - d1s[c] * setback;
            corner.t2 = p + d2s[c] * setback;
            const Vertex2D left(-d1s[c].y, d1s[c].x);
            corner.c = corner.t1 + left * (turns[c] > 0.f ? plan.radius : -plan.radius);
            corner.a0 = atan2f(corner.t1.y - corner.c.y, corner.t1.x - corner.c.x);
            corner.sweep = turns[c];
            corner.outer = outer[c];
            corner.inner = inner[c];
            plan.corners.push_back(corner);
         }
         // The two legs of a turn-back must stay a ramp width apart (centre lines), so their walls do not overlap
         if (fabsf(totalTurn) > ANGTORAD(150.f))
         {
            const float dist = SegmentDistance(cpPos(s.m_first - 1), plan.corners.front().t1, plan.corners.back().t2, cpPos(s.m_last + 1));
            if (status)
               (*status)[si].m_legGap = dist - wMax;
            if (dist < wMax)
            {
               char buf[256];
               snprintf(buf, sizeof(buf), "The two legs are %.0f VPU apart, under the ramp width (%.0f): move them apart", dist, wMax);
               report(si, buf);
               continue;
            }
         }
         prevLast = s.m_last;
         plans.push_back(std::move(plan));
      }
   }

   // ---- Path: the curve with the stretch of every planned hairpin replaced by straight legs and its arcs
   struct PathVertex
   {
      Vertex3Ds p;
      bool cp;
      int arc; // index into arcs, -1 for none
      int step; // 0 .. arc steps
   };
   struct Arc
   {
      Vertex2D c;
      float radius, a0, sweep;
      int steps, outer, inner;
      size_t section;
   };
   vector<PathVertex> path;
   vector<Arc> arcs;
   {
      int v = 0;
      const auto push = [&path](const PathVertex &pv)
      {
         if (!path.empty() && fabsf(path.back().p.x - pv.p.x) < 1e-4f && fabsf(path.back().p.y - pv.p.y) < 1e-4f)
            return; // coincident: keep the first (identical position)
         path.push_back(pv);
      };
      for (const Plan &plan : plans)
      {
         const int vA = cpVert[plan.first - 1], vB = cpVert[plan.last + 1];
         for (; v <= vA; v++)
            push({ vv[v], vv[v].controlPoint, -1, 0 });
         const size_t start = path.size();
         for (const Corner &corner : plan.corners)
         {
            const int steps = max(corner.outer, corner.inner);
            const int arcIndex = (int)arcs.size();
            arcs.push_back({ corner.c, plan.radius, corner.a0, corner.sweep, steps, corner.outer, corner.inner, plan.section });
            for (int j = 0; j <= steps; j++)
            {
               const float a = corner.a0 + corner.sweep * (float)j / (float)steps;
               push({ Vertex3Ds(corner.c.x + plan.radius * cosf(a), corner.c.y + plan.radius * sinf(a), 0.f), false, arcIndex, j });
            }
         }
         // Height offsets (point z) interpolate by length between the two ends of the replaced stretch
         const Vertex2D a(vv[vA].x, vv[vA].y), b(vv[vB].x, vv[vB].y);
         float total = 0.f;
         Vertex2D prev = a;
         for (size_t i = start; i < path.size(); i++)
         {
            total += (Vertex2D(path[i].p.x, path[i].p.y) - prev).Length();
            prev = Vertex2D(path[i].p.x, path[i].p.y);
         }
         total += (b - prev).Length();
         float acc = 0.f;
         prev = a;
         for (size_t i = start; i < path.size(); i++)
         {
            acc += (Vertex2D(path[i].p.x, path[i].p.y) - prev).Length();
            prev = Vertex2D(path[i].p.x, path[i].p.y);
            path[i].p.z = vv[vA].z + (vv[vB].z - vv[vA].z) * ((total > 0.f) ? acc / total : 0.f);
         }
         v = vB;
         if (status)
            (*status)[plan.section].m_applied = true;
      }
      for (; v < (int)vv.size(); v++)
         push({ vv[v], vv[v].controlPoint, -1, 0 });
   }
   const int cvertex = (int)path.size();
   if (cvertex < 2)
      return false;

   // ---- Rows over the path, computed as GetRampVertex does for a plain flat ramp
   rows = SectionRows();
   rows.right.resize(cvertex);
   rows.left.resize(cvertex);
   rows.middle.resize(cvertex);
   rows.height.resize(cvertex);
   rows.ratio.resize(cvertex);
   rows.cross.resize(cvertex);
   rows.wall.assign(cvertex, true);
   vector<float> widths(cvertex), lengths(cvertex);
   float totallength = 0.f;
   for (int i = 0; i < cvertex - 1; i++)
      totallength += (Vertex2D(path[i].p.x, path[i].p.y) - Vertex2D(path[i + 1].p.x, path[i + 1].p.y)).Length();
   float currentlength = 0.f;
   for (int i = 0; i < cvertex; i++)
   {
      const Vertex3Ds &vprev = path[(i > 0) ? i - 1 : i].p;
      const Vertex3Ds &vnext = path[(i < (cvertex - 1)) ? i + 1 : i].p;
      const Vertex3Ds &vmiddle = path[i].p;
      Vertex2D vnormal;
      {
         Vertex2D v1normal(vprev.y - vmiddle.y, vmiddle.x - vprev.x);
         Vertex2D v2normal(vmiddle.y - vnext.y, vnext.x - vmiddle.x);
         if (i == (cvertex - 1))
         {
            v1normal.Normalize();
            vnormal = v1normal;
         }
         else if (i == 0)
         {
            v2normal.Normalize();
            vnormal = v2normal;
         }
         else
         {
            v1normal.Normalize();
            v2normal.Normalize();
            if (fabsf(v1normal.x - v2normal.x) < 0.0001f && fabsf(v1normal.y - v2normal.y) < 0.0001f)
               vnormal = v1normal;
            else
            {
               const float A = vprev.y - vmiddle.y;
               const float B = vmiddle.x - vprev.x;
               const float C = A * (v1normal.x - vprev.x) + B * (v1normal.y - vprev.y);
               const float D = vnext.y - vmiddle.y;
               const float E = vmiddle.x - vnext.x;
               const float F = D * (v2normal.x - vnext.x) + E * (v2normal.y - vnext.y);
               const float det = A * E - B * D;
               const float inv_det = (det != 0.0f) ? 1.0f / det : 0.0f;
               vnormal.x = vmiddle.x - (B * F - E * C) * inv_det;
               vnormal.y = vmiddle.y - (C * D - A * F) * inv_det;
            }
         }
      }
      currentlength += (Vertex2D(vprev.x, vprev.y) - Vertex2D(vmiddle.x, vmiddle.y)).Length();
      lengths[i] = currentlength;
      const float percentage = (totallength != 0.f) ? currentlength / totallength : 0.f;
      widths[i] = percentage * (m_d.m_widthtop - m_d.m_widthbottom) + m_d.m_widthbottom;
      rows.height[i] = vmiddle.z + percentage * (m_d.m_heighttop - m_d.m_heightbottom) + m_d.m_heightbottom;
      if (path[i].cp)
      {
         RenderVertex3D rv;
         rv.set(vmiddle);
         AssignHeightToControlPoint(rv, rows.height[i]);
      }
      rows.ratio[i] = 1.0f - percentage;
      rows.cross[i] = path[i].cp;
      rows.middle[i] = Vertex2D(vmiddle.x, vmiddle.y);
      rows.right[i] = rows.middle[i] + (widths[i] * 0.5f) * vnormal;
      rows.left[i] = rows.middle[i] - (widths[i] * 0.5f) * vnormal;
   }

   // ---- Hairpin arcs: exact concentric walls, each side resampled to its own face count (steps of the coarser side repeat a point)
   {
      vector<float> arcWidthSum(arcs.size(), 0.f);
      vector<int> arcWidthCount(arcs.size(), 0);
      for (int i = 0; i < cvertex; i++)
         if (path[i].arc >= 0)
         {
            arcWidthSum[path[i].arc] += widths[i];
            arcWidthCount[path[i].arc]++;
         }
      for (int i = 0; i < cvertex; i++)
      {
         if (path[i].arc < 0)
            continue;
         const Arc &arc = arcs[path[i].arc];
         const float w = arcWidthSum[path[i].arc] / (float)max(1, arcWidthCount[path[i].arc]); // one width per arc keeps both walls concentric
         const bool rightIsOuter = (rows.right[i] - rows.middle[i]).Dot(rows.middle[i] - arc.c) > 0.f;
         const auto sidePoint = [&arc, w, &path, i](int count, bool outer)
         {
            const int k = (int)lroundf((float)path[i].step * (float)count / (float)arc.steps);
            const float a = arc.a0 + arc.sweep * (float)k / (float)count;
            const float r = arc.radius + (outer ? 0.5f * w : -0.5f * w);
            return Vertex2D(arc.c.x + r * cosf(a), arc.c.y + r * sinf(a));
         };
         rows.right[i] = sidePoint(rightIsOuter ? arc.outer : arc.inner, rightIsOuter);
         rows.left[i] = sidePoint(rightIsOuter ? arc.inner : arc.outer, !rightIsOuter);
      }
   }

   // ---- Entry flap: replaces the rows from the ramp foot up to its landing point, plus rows ahead of the foot (no walls there)
   bool flapDone = false;
   for (size_t si = 0; si < sections.size(); si++)
   {
      const RampSection &s = sections[si];
      if (s.m_kind != RampSection::EntryFlap)
         continue;
      if (flapDone)
      {
         report(si, "Only one entry interface per ramp"s);
         continue;
      }
      if (s.m_first != 0)
      {
         report(si, "An entry interface starts at the ramp's first point"s);
         continue;
      }
      const int n = (int)rows.middle.size();
      vector<float> sv(n);
      sv[0] = 0.f;
      for (int i = 1; i < n; i++)
         sv[i] = sv[i - 1] + (rows.middle[i] - rows.middle[i - 1]).Length();
      const float total = sv[n - 1];
      float bMax = total * 0.9f;
      for (int i = 0; i < n; i++)
         if (path[i].arc >= 0)
         {
            bMax = min(bMax, sv[i]); // never into a hairpin
            break;
         }
      const float a = max(0.f, s.m_flapAhead);
      const float b = clamp(s.m_flapUp, 0.f, bMax);
      if (b < 1.f)
      {
         report(si, "The flap must land at least 1 VPU up the ramp"s);
         continue;
      }
      const auto at = [&sv, n](float d, int &seg, float &t)
      {
         seg = 0;
         while (seg < n - 2 && sv[seg + 1] < d)
            seg++;
         const float len = sv[seg + 1] - sv[seg];
         t = (len > 1e-6f) ? clamp((d - sv[seg]) / len, 0.f, 1.f) : 0.f;
      };
      int seg;
      float t;
      at(b, seg, t);
      const float z0 = rows.height[0];
      const float z2 = rows.height[seg] + (rows.height[seg + 1] - rows.height[seg]) * t;
      const float L = a + b;
      const float cs = (z2 - z0) / L;
      const float f = clamp(s.m_flapBowPos, 0.1f, 0.9f);
      float bow = max(0.f, s.m_flapBow);
      // Caps: the front edge never starts more than about 6 degrees downhill, and the flap never sinks more than 5 VPU below the foot
      bow = min(bow, max(0.f, (cs + tanf(ANGTORAD(6.f))) * f * L * 0.5f));
      for (int j = 1; j < 128; j++)
      {
         const float u = (float)j / 128.f;
         const float g = FlapBowShape(u, f);
         if (g > 1e-4f)
            bow = min(bow, max(0.f, (cs * u * L + 5.f) / g));
      }
      const int faces = (bow > 0.01f) ? 8 : 1;
      vector<float> xs;
      for (int j = 0; j <= faces; j++)
         xs.push_back(L * (float)j / (float)faces);
      if (a > 0.f && std::ranges::none_of(xs, [a](float x) { return fabsf(x - a) < 1e-3f; }))
      {
         xs.push_back(a);
         std::ranges::sort(xs);
      }
      Vertex2D dir0 = rows.middle[1] - rows.middle[0];
      dir0.NormalizeSafe();
      SectionRows out;
      const auto add = [&out](const Vertex2D &r, const Vertex2D &l, const Vertex2D &m, float h, float ratio, bool cross, bool wall)
      {
         out.right.push_back(r);
         out.left.push_back(l);
         out.middle.push_back(m);
         out.height.push_back(h);
         out.ratio.push_back(ratio);
         out.cross.push_back(cross);
         out.wall.push_back(wall);
      };
      float zMin = z0;
      for (const float x : xs)
      {
         const float d = x - a; // distance along the ramp from its foot (negative ahead of it)
         const float h = z0 + cs * x - bow * FlapBowShape(x / L, f);
         zMin = min(zMin, h);
         const float ratio = 1.f - d / max(total, 1e-6f);
         if (d <= 1e-3f)
         {
            const Vertex2D off = dir0 * min(d, 0.f);
            const bool foot = d > -1e-3f;
            if (foot && a > 0.f)
               add(rows.right[0] + off, rows.left[0] + off, rows.middle[0] + off, h, ratio, true, false); // walls start at the foot
            add(rows.right[0] + off, rows.left[0] + off, rows.middle[0] + off, h, ratio, foot, foot);
         }
         else
         {
            at(d, seg, t);
            add(rows.right[seg] + (rows.right[seg + 1] - rows.right[seg]) * t, rows.left[seg] + (rows.left[seg + 1] - rows.left[seg]) * t,
               rows.middle[seg] + (rows.middle[seg + 1] - rows.middle[seg]) * t, h, ratio, false, true);
         }
      }
      for (int i = 0; i < n; i++)
         if (sv[i] > b + 1e-3f)
            add(rows.right[i], rows.left[i], rows.middle[i], rows.height[i], rows.ratio[i], rows.cross[i], rows.wall[i]);
      rows = std::move(out);
      flapDone = true;
      if (status)
      {
         RampSectionStatus &st = (*status)[si];
         st.m_applied = true;
         st.m_bow = bow;
         st.m_flapUp = b;
         st.m_entryAngle = RADTOANG(atanf(cs - 2.f * bow / (f * L)));
         st.m_sink = z0 - zMin;
         if (bow + 0.01f < s.m_flapBow)
            st.m_message = "Bow capped (front edge at most 6 deg downhill, at most 5 VPU below the foot)";
         else if (b + 0.01f < s.m_flapUp)
            st.m_message = "Landing limited to the straight run before a hairpin / 90% of the ramp";
      }
   }
   return rows.middle.size() >= 2;
}

vector<RampSectionStatus> Ramp::GetSectionStatus() const
{
   vector<RampSectionStatus> status;
   if (m_d.m_type != RampTypeFlat)
   {
      status.assign(m_d.m_sections.size(), RampSectionStatus());
      for (RampSectionStatus &st : status)
         st.m_message = "Sections apply to flat ramps only";
      return status;
   }
   SectionRows rows;
   BuildSectionedRows(rows, &status);
   return status;
}

bool Ramp::CanAddEntryInterface() const
{
   return m_d.m_type == RampTypeFlat && std::ranges::none_of(m_d.m_sections, [](const RampSection &s) { return s.m_kind == RampSection::EntryFlap; });
}

bool Ramp::AddEntryInterface()
{
   if (!CanAddEntryInterface())
      return false;
   RampSection s;
   s.m_kind = RampSection::EntryFlap;
   s.m_first = s.m_last = 0;
   const float w = m_d.m_widthbottom;
   s.m_flapAhead = clamp(w * 0.75f, 20.f, 120.f);
   s.m_flapUp = clamp(w * 0.75f, 20.f, 120.f);
   m_d.m_sections.push_back(s);
   return true;
}

bool Ramp::CanAddHairpin(int first, int last) const
{
   const int npts = (int)m_curve.GetPoints().size();
   if (m_d.m_type != RampTypeFlat || first < 1 || last > npts - 2 || first > last)
      return false;
   return std::ranges::none_of(m_d.m_sections,
      [first, last](const RampSection &s) { return s.m_kind == RampSection::Hairpin && s.m_first <= last + 1 && s.m_last >= first - 1; });
}

bool Ramp::AddHairpin(int first, int last)
{
   if (!CanAddHairpin(first, last))
      return false;
   RampSection s;
   s.m_kind = RampSection::Hairpin;
   s.m_first = first;
   s.m_last = last;
   const float wMax = max(m_d.m_widthbottom, m_d.m_widthtop);
   s.m_radius = max(wMax * 0.5f + 1.f, wMax * 0.75f);
   s.m_outerFaces = 12;
   s.m_innerFaces = 9;
   m_d.m_sections.push_back(s);
   return true;
}

void Ramp::ShiftSectionAnchors(int index, int delta)
{
   for (RampSection &s : m_d.m_sections)
   {
      if (s.m_kind == RampSection::EntryFlap)
         continue; // always at the start of the ramp
      if (delta > 0)
      {
         if (s.m_first >= index)
            s.m_first++;
         if (s.m_last >= index)
            s.m_last++;
      }
      else
      {
         if (s.m_first > index)
            s.m_first--;
         if (s.m_last >= index)
            s.m_last--;
      }
   }
   std::erase_if(m_d.m_sections, [](const RampSection &s) { return s.m_last < s.m_first; });
}

#pragma endregion

// Ported at: VisualPinball.Engine/VPT/Ramp/Ramp.cs

float Ramp::GetSurfaceHeight(float x, float y) const
{
   vector<RenderVertex3D> vvertex;
   GetCentralCurve(vvertex);

   int iSeg;
   Vertex2D vOut;
   ClosestPointOnPolygon(vvertex, Vertex2D(x, y), vOut, iSeg, false);

   if (iSeg == -1)
      return 0.0f; // Object is not on ramp path

   // Go through vertices (including iSeg itself) counting control points until iSeg
   float totallength = 0.f;
   float startlength = 0.f;

   const int cvertex = (int)vvertex.size();
   for (int i2 = 1; i2 < cvertex; i2++)
   {
      const float dx = vvertex[i2].x - vvertex[i2 - 1].x;
      const float dy = vvertex[i2].y - vvertex[i2 - 1].y;
      const float len = sqrtf(dx*dx + dy*dy);
      if (i2 <= iSeg)
         startlength += len;

      totallength += len;
   }

   const float dx = vOut.x - vvertex[iSeg].x;
   const float dy = vOut.y - vvertex[iSeg].y;
   const float len = sqrtf(dx*dx + dy*dy);
   startlength += len; // Add the distance the object is between the two closest polyline segments.  Matters mostly for straight edges. Z does not respect that yet!

   const float topHeight = m_d.m_heighttop;
   const float bottomHeight = m_d.m_heightbottom;

   return vvertex[iSeg].z + ((totallength != 0.f) ? startlength / totallength : 0.f) * (topHeight - bottomHeight) + bottomHeight;
}


#pragma region Physics

// Ported at: VisualPinball.Engine/VPT/Ramp/RampHitGenerator.cs

void Ramp::PhysicSetup(PhysicsEngine* physics, const bool isUI)
{
   if (!isUI && GetPartGroup() != nullptr && GetPartGroup()->GetReferenceSpace() != PartGroupData::SpaceReference::SR_PLAYFIELD)
      return;

   float wallheightright, wallheightleft;
   if (m_d.m_type == RampTypeFlat)
   {
      wallheightright = m_d.m_rightwallheight;
      wallheightleft = m_d.m_leftwallheight;
   }
   else if (m_d.m_type == RampType1Wire) //add check for 1 wire
   {
      // backwards compatible physics
      wallheightright = 31.0f;
      wallheightleft = 31.0f;
   }
   else if (m_d.m_type == RampType2Wire)
   {
      // backwards compatible physics
      wallheightright = 31.0f;
      wallheightleft = 31.0f;
   }
   else if (m_d.m_type == RampType4Wire)
   {
      wallheightright = 62.0f;
      wallheightleft = 62.0f;
   }
   else if (m_d.m_type == RampType3WireRight)
   {
      wallheightright = 62.0f;
      wallheightleft = (float)(6 + 12.5);
   }
   else if (m_d.m_type == RampType3WireLeft)
   {
      wallheightright = (float)(6 + 12.5);
      wallheightleft = 62.0f;
   }
   else
   {
      ShowError("Unknown Ramp type");
      return;
   }

   // The 10.9 physics profile builds the walls as exact sloped quads (see AddWallQuad) instead of
   // the legacy rectangle approximation which leaks the ball on steep ramp sections (see below)
   const bool profile109 = physics->GetPhysicProfile() == PhysicProfile::V10_9;

   float *rgheight1;
   int cvertex;
   bool *rgwall; // only for a sectioned ramp: a wall segment exists between two consecutive rows that both have one
   const Vertex2D *const rgvLocal = GetRampVertex(cvertex, &rgheight1, nullptr, nullptr, nullptr, HIT_SHAPE_DETAIL_LEVEL, true, &rgwall);
   m_physicsSectioned = rgwall != nullptr;
   // Sectioned ramps: no wall where a row has none, and no wall on a zero length segment (a hairpin side with fewer faces repeats its points)
   const auto hasWall = [rgwall, rgvLocal](int r0, int r1, int o0, int o1)
   { return rgwall == nullptr || (rgwall[r0] && rgwall[r1] && (rgvLocal[o0] - rgvLocal[o1]).LengthSquared() > 1e-6f); };

   // Add line segments for right ramp wall.
   if (wallheightright > 0.f)
   {
      if (profile109)
      {
         Vertex3Ds prevNormal(0.f, 0.f, 0.f); // zero: no previous quad, the first start edge is exposed
         for (int i = 0; i < (cvertex - 1); i++)
         {
            if (!hasWall(i, i + 1, i, i + 1))
            {
               prevNormal = Vertex3Ds(0.f, 0.f, 0.f);
               continue;
            }
            const bool lastQuad = (i == (cvertex - 2)) || !hasWall(i + 1, i + 2, i + 1, i + 2);
            prevNormal = AddWallQuad(physics, rgvLocal[i], rgheight1[i], rgvLocal[i + 1], rgheight1[i + 1], wallheightright, prevNormal, i == 0, lastQuad, isUI);
         }
      }
      else
         for (int i = 0; i < (cvertex - 1); i++)
         {
            if (!hasWall(i, i + 1, i, i + 1))
               continue;
            const Vertex2D &pv2 = rgvLocal[i];
            const Vertex2D &pv3 = rgvLocal[i + 1];

            AddWallLineSeg(physics, pv2, pv3, (i > 0), rgheight1[i], rgheight1[i + 1], wallheightright, isUI);
            AddWallLineSeg(physics, pv3, pv2, (i < (cvertex - 2)), rgheight1[i], rgheight1[i + 1], wallheightright, isUI);

            // add joints at start and end of right wall
            if (i == 0)
               AddJoint2D(physics, pv2, rgheight1[0], rgheight1[0] + wallheightright, isUI);
            if (i == cvertex - 2)
               AddJoint2D(physics, pv3, rgheight1[cvertex - 1], rgheight1[cvertex - 1] + wallheightright, isUI);
         }
   }

   // Add line segments for left ramp wall.
   if (wallheightleft > 0.f)
   {
      if (profile109)
      {
         Vertex3Ds prevNormal(0.f, 0.f, 0.f);
         for (int i = 0; i < (cvertex - 1); i++)
         {
            if (!hasWall(cvertex - i - 1, cvertex - i - 2, cvertex + i, cvertex + i + 1))
            {
               prevNormal = Vertex3Ds(0.f, 0.f, 0.f);
               continue;
            }
            const bool lastQuad = (i == (cvertex - 2)) || !hasWall(cvertex - i - 2, cvertex - i - 3, cvertex + i + 1, cvertex + i + 2);
            prevNormal = AddWallQuad(physics, rgvLocal[cvertex + i], rgheight1[cvertex - i - 1], rgvLocal[cvertex + i + 1], rgheight1[cvertex - i - 2], wallheightleft, prevNormal, i == 0,
               lastQuad, isUI);
         }
      }
      else
         for (int i = 0; i < (cvertex - 1); i++)
         {
            if (!hasWall(cvertex - i - 1, cvertex - i - 2, cvertex + i, cvertex + i + 1))
               continue;
            const Vertex2D &pv2 = rgvLocal[cvertex + i];
            const Vertex2D &pv3 = rgvLocal[cvertex + i + 1];

            AddWallLineSeg(physics, pv2, pv3, (i > 0), rgheight1[cvertex - i - 2], rgheight1[cvertex - i - 1], wallheightleft, isUI);
            AddWallLineSeg(physics, pv3, pv2, (i < (cvertex - 2)), rgheight1[cvertex - i - 2], rgheight1[cvertex - i - 1], wallheightleft, isUI);

            // add joints at start and end of left wall
            if (i == 0)
               AddJoint2D(physics, pv2, rgheight1[cvertex - 1], rgheight1[cvertex - 1] + wallheightleft, isUI);
            if (i == cvertex - 2)
               AddJoint2D(physics, pv3, rgheight1[0], rgheight1[0] + wallheightleft, isUI);
         }
   }

   // Add hit triangles for the ramp floor.
   {
      const HitTriangle *ph3dpolyOld = nullptr;
      const Vertex2D *pv3, *pv4;

      for (int i = 0; i < (cvertex - 1); i++)
      {
         /*
         * Layout of one ramp quad seen from above, ramp direction is bottom to top:
         *
         *    3 - - 4
         *    | \   |
         *    |   \ |
         *    2 - - 1
         */
         const Vertex2D *pv1, *pv2;
         pv1 = &rgvLocal[i]; // i-th right
         pv2 = &rgvLocal[cvertex * 2 - i - 1]; // i-th left
         pv3 = &rgvLocal[cvertex * 2 - i - 2]; // (i+1)-th left
         pv4 = &rgvLocal[i + 1]; // (i+1)-th right

         {
            // left ramp floor triangle, CCW order
            const Vertex3Ds rgv3D[3] = { Vertex3Ds(pv2->x, pv2->y, rgheight1[i]), Vertex3Ds(pv1->x, pv1->y, rgheight1[i]), Vertex3Ds(pv3->x, pv3->y, rgheight1[i + 1]) };

            // add joint for left edge
            AddJoint(physics, rgv3D[0], rgv3D[2], isUI);

            HitTriangle *const ph3dpoly = new HitTriangle(this, rgv3D, physics->GetPhysicProfile()); //!! this is not efficient at all, use native triangle-soup directly somehow

            if (ph3dpoly->IsDegenerate()) // degenerate triangles happen if width is 0 at some point
            {
               delete ph3dpoly;
               if (!m_physicsSectioned) // a sectioned ramp's degenerate triangles are on purpose (repeated hairpin points): the floor stays continuous
                  ph3dpolyOld = nullptr;
            }
            else
            {
               SetupHitObject(physics, ph3dpoly, isUI);

               // Add joint between this tri and the previous (or joint for starting edge of ramp for the first tri, or after a degenrate)
               CheckJoint(physics, ph3dpolyOld, ph3dpoly, isUI);
               ph3dpolyOld = ph3dpoly;
            }
         }

         {
            // right ramp floor triangle, CCW order
            const Vertex3Ds rgv3D[3] = { Vertex3Ds(pv3->x, pv3->y, rgheight1[i + 1]), Vertex3Ds(pv1->x, pv1->y, rgheight1[i]), Vertex3Ds(pv4->x, pv4->y, rgheight1[i + 1]) };

            // add joint for right edge
            AddJoint(physics, rgv3D[1], rgv3D[2], isUI);

            HitTriangle *const ph3dpoly = new HitTriangle(this, rgv3D, physics->GetPhysicProfile());
            if (ph3dpoly->IsDegenerate())
            {
               delete ph3dpoly;
               if (!m_physicsSectioned)
                  ph3dpolyOld = nullptr;
            }
            else
            {
               SetupHitObject(physics, ph3dpoly, isUI);

               CheckJoint(physics, ph3dpolyOld, ph3dpoly, isUI);
               ph3dpolyOld = ph3dpoly;
            }
         }
      }

      if (cvertex >= 2)
      {
         // add joint for final edge of ramp
         const Vertex3Ds v1(pv4->x, pv4->y, rgheight1[cvertex - 1]);
         const Vertex3Ds v2(pv3->x, pv3->y, rgheight1[cvertex - 1]);
         AddJoint(physics, v1, v2, isUI);
      }
   }

   // add outside bottom,
   // joints at the intersections are not needed since the inner surface has them
   // this surface is identical... except for the direction of the normal face.
   // hence the joints protect both surface edges from having a fall through

   for (int i = 0; i < (cvertex - 1); i++)
   {
      // see sketch above
      const Vertex2D &pv1 = rgvLocal[i];
      const Vertex2D &pv2 = rgvLocal[cvertex * 2 - i - 1];
      const Vertex2D &pv3 = rgvLocal[cvertex * 2 - i - 2];
      const Vertex2D &pv4 = rgvLocal[i + 1];

      {
         // left ramp triangle, order CW
         const Vertex3Ds rgv3D[3] = { Vertex3Ds(pv1.x, pv1.y, rgheight1[i]), Vertex3Ds(pv2.x, pv2.y, rgheight1[i]), Vertex3Ds(pv3.x, pv3.y, rgheight1[i + 1]) };

         HitTriangle *const ph3dpoly = new HitTriangle(this, rgv3D, physics->GetPhysicProfile());
         if (ph3dpoly->IsDegenerate())
         {
            delete ph3dpoly;
         }
         else
         {
            SetupHitObject(physics, ph3dpoly, isUI);
         }
      }
      {
         // right ramp triangle, order CW
         const Vertex3Ds rgv3D[3] = { Vertex3Ds(pv3.x, pv3.y, rgheight1[i + 1]), Vertex3Ds(pv4.x, pv4.y, rgheight1[i + 1]), Vertex3Ds(pv1.x, pv1.y, rgheight1[i]) };

         HitTriangle *const ph3dpoly = new HitTriangle(this, rgv3D, physics->GetPhysicProfile());
         if (ph3dpoly->IsDegenerate())
         {
            delete ph3dpoly;
         }
         else
         {
            SetupHitObject(physics, ph3dpoly, isUI);
         }
      }
   }

   delete[] rgheight1;
   delete[] rgvLocal;
   delete[] rgwall;
   m_physicsSectioned = false;
}

void Ramp::PhysicRelease(PhysicsEngine* physics, const bool isUI)
{
   if (!isUI)
      m_vhoCollidable.clear();
}

void Ramp::CheckJoint(PhysicsEngine *physics, const HitTriangle *const ph3d1, const HitTriangle *const ph3d2, const bool isUI)
{
   if (ph3d1) // may be null in case of degenerate triangles
   {
      const Vertex3Ds vjointnormal = CrossProduct(ph3d1->m_normal, ph3d2->m_normal);
      if (vjointnormal.LengthSquared() < 1e-8f)
         return;  // coplanar triangles need no joints
   }

   // By convention of the calling function, points 1 [0] and 2 [1] of the second polygon will
   // be the common-edge points
   AddJoint(physics, ph3d2->m_rgv[0], ph3d2->m_rgv[1], isUI);
}

void Ramp::AddJoint(PhysicsEngine* physics, const Vertex3Ds& v1, const Vertex3Ds& v2, const bool isUI)
{
   if (m_physicsSectioned && (v1 - v2).LengthSquared() < 1e-6f)
      return; // repeated point of a sectioned ramp
   SetupHitObject(physics, new HitLine3D(this, v1, v2), isUI);
}

void Ramp::AddJoint2D(PhysicsEngine* physics, const Vertex2D& p, const float zlow, const float zhigh, const bool isUI)
{
   SetupHitObject(physics, new HitLineZ(this, p, zlow, zhigh), isUI);
}

void Ramp::AddWallLineSeg(PhysicsEngine* physics, const Vertex2D &pv1, const Vertex2D &pv2, const bool pv3_exists, const float height1, const float height2, const float wallheight, const bool isUI)
{
   //!! Hit-walls are still done via 2D line segments with only a single lower and upper border, so the wall will always reach below and above the actual ramp -between- two points of the ramp
   // (kept for the 10.8 physics profile; the 10.9 profile builds the exact sloped wall in AddWallQuad instead)

   // Thus, subdivide until at some point the approximation error is 'subtle' enough so that one will usually not notice (i.e. dependent on ball size)
   if (height2-height1 > (float)(2.0*PHYS_SKIN)) //!! use ballsize
   {
      AddWallLineSeg(physics, pv1, (pv1+pv2)*0.5f, pv3_exists, height1, (height1+height2)*0.5f, wallheight, isUI);
      AddWallLineSeg(physics, (pv1+pv2)*0.5f, pv2, true,       (height1+height2)*0.5f, height2, wallheight, isUI);
   }
   else
   {
      LineSeg * const plineseg = new LineSeg(this, pv1, pv2, height1, height2+wallheight);

      SetupHitObject(physics, plineseg, isUI);

      if (pv3_exists)
         AddJoint2D(physics, pv1, height1, height2+wallheight, isUI);
   }
}

// 10.9 physics profile: the wall between two consecutive ramp outline points is an exact sloped quad
// (its bottom edge is the ramp edge itself), instead of the LineSeg rectangle approximation whose
// subdivided height windows could be swapped to the wrong segment end on one direction, leaving dead
// zones where the ball escaped through the wall on steep sections. Returns the quad's face normal for
// the caller's seam test (a zero normal marks a degenerate quad, treated as a gap in the wall strip).
Vertex3Ds Ramp::AddWallQuad(PhysicsEngine *physics, const Vertex2D &pv1, const float height1, const Vertex2D &pv2, const float height2, const float wallheight, const Vertex3Ds &prevNormal,
   const bool addStartJoint, const bool addEndJoint, const bool isUI)
{
   const Vertex3Ds quad[4] = { Vertex3Ds(pv1.x, pv1.y, height1), // bottom edge at pv1
      Vertex3Ds(pv2.x, pv2.y, height2), // bottom edge at pv2
      Vertex3Ds(pv2.x, pv2.y, height2 + wallheight), // top edge at pv2
      Vertex3Ds(pv1.x, pv1.y, height1 + wallheight) }; // top edge at pv1

   // HitTriangle is single-sided: emit the quad twice, once per winding
   static const unsigned int quadTris[4][3] = { { 0, 1, 2 }, { 0, 2, 3 }, { 0, 2, 1 }, { 0, 3, 2 } };
   Vertex3Ds normal(0.f, 0.f, 0.f);
   for (const unsigned int (&tri)[3] : quadTris)
   {
      const Vertex3Ds rgv3D[3] = { quad[tri[0]], quad[tri[1]], quad[tri[2]] };
      HitTriangle *const ph3dpoly = new HitTriangle(this, rgv3D, physics->GetPhysicProfile());
      if (ph3dpoly->IsDegenerate())
         delete ph3dpoly;
      else
      {
         normal = ph3dpoly->m_normal;
         SetupHitObject(physics, ph3dpoly, isUI);
      }
   }

   if (normal.IsZero())
      return normal; // degenerate quad (zero-length edge): the next quad's start edge is exposed

   AddJoint(physics, quad[3], quad[2], isUI); // the top edge is always exposed

   // The start vertical edge is exposed at the strip start, after a degenerate gap, or when the
   // previous quad forms a crease (the floor uses the same coplanarity test in CheckJoint)
   if (addStartJoint || prevNormal.IsZero() || CrossProduct(prevNormal, normal).LengthSquared() >= 1e-8f)
      AddJoint(physics, quad[0], quad[3], isUI);
   if (addEndJoint)
      AddJoint(physics, quad[1], quad[2], isUI);

   return normal;
}

void Ramp::SetupHitObject(PhysicsEngine* physics, HitObject *obj, const bool isUI)
{
   const Material * const mat = m_ptable->GetMaterial(m_d.m_szPhysicsMaterial);
   if (!m_d.m_overwritePhysics)
   {
      obj->m_elasticity = mat->m_fElasticity;
      obj->SetFriction(mat->m_fFriction);
      obj->m_scatter = ANGTORAD(mat->m_fScatterAngle);
   }
   else
   {
      obj->m_elasticity = m_d.m_elasticity;
      obj->SetFriction(m_d.m_friction);
      obj->m_scatter = ANGTORAD(m_d.m_scatter);
   }

   obj->m_threshold = m_d.m_threshold;
   // the ramp is of type ePrimitive for triggering the event in HitTriangle::Collide()
   obj->m_ObjType = ePrimitive;
   obj->m_enabled = isUI ? true : m_d.m_collidable;
   obj->m_obj = (IFireEvents*)this;
   obj->m_fe = m_d.m_hitEvent;

   physics->AddCollider(obj, isUI);
   if (!isUI)
      m_vhoCollidable.push_back(obj); //remember hit components of primitive
}

#pragma endregion


#pragma region Rendering

void Ramp::RenderSetup(Renderer *renderer)
{
   assert(m_renderer == nullptr);
   m_renderer = renderer;
   UpdateBounds();
}

void Ramp::RenderRelease()
{
   assert(m_renderer != nullptr);
   m_meshBuffer = nullptr;
   m_meshEdgeBuffer = nullptr;
   m_dynamicVertexBufferRegenerate = true;
   m_renderer = nullptr;
}

void Ramp::UpdateAnimation(const float diff_time_msec)
{
   assert(m_renderer != nullptr);
}

void Ramp::Render(const unsigned int renderMask)
{
   assert(m_renderer != nullptr);
   assert(!m_desktopBackdrop);
   const bool isStaticOnly = renderMask & Renderer::STATIC_ONLY;
   const bool isDynamicOnly = renderMask & Renderer::DYNAMIC_ONLY;
   const bool isReflectionPass = renderMask & Renderer::REFLECTION_PASS;
   const bool isUIPass = renderMask & Renderer::UI_EDGES || renderMask & Renderer::UI_FILL;
   TRACE_FUNCTION();

   const Material *const mat = m_ptable->GetMaterial(m_d.m_szMaterial);
   if (!m_d.m_visible
    || (!isUIPass && mat == nullptr)
    || (isReflectionPass && !m_d.m_reflectionEnabled)
    || (isStaticOnly && mat->m_bOpacityActive)
    || (isDynamicOnly && !mat->m_bOpacityActive))
      return;

   if (m_d.m_widthbottom == 0.0f && m_d.m_widthtop == 0.0f)
   {
      m_dynamicVertexBufferRegenerate = false;
      return;
   }

   if (IsHabitrail())
   {
      if (!m_meshBuffer || m_dynamicVertexBufferRegenerate)
         PrepareHabitrail();
      if (!m_meshBuffer)
         return; // Nothing to draw on a degenerate curve

      RenderMeshDesc body;
      body.mesh = m_meshBuffer;
      body.indexCount = m_numIndices;
      body.sortCenter = m_boundingSphereCenter;
      body.depthBias = m_d.m_depthBias;
      body.uiEdgeMesh = &m_meshEdgeBuffer;
      body.edgeMode = UIEdgeMode::Quads;
      body.edgeTriIndices = m_meshIndices.data();
      body.edgeTriCount = m_meshIndices.size();
      body.edgeTriIndices16 = true;

      VisualMaterial material;
      if (!isUIPass)
      {
         Texture *const pin = m_ptable->GetImage(m_d.m_szImage);
         /* TODO: This is a misnomer right now, but clamp fixes some visual glitches (single-pixel lines)
          * with transparent textures. Probably the option should simply be renamed to ImageModeClamp,
          * since the texture coordinates always stay within [0,1] anyway. */
         const SamplerAddressMode sam = m_d.m_imagealignment == ImageModeWrap ? SamplerAddressMode::SA_CLAMP : SamplerAddressMode::SA_REPEAT;
         m_renderer->m_renderDevice->ResetRenderState();
         m_renderer->m_renderDevice->SetRenderState(RenderState::CULLMODE, RenderState::CULL_NONE);
         material.material = mat;
         material.image = pin;
         material.filter = SamplerFilter::SF_TRILINEAR;
         material.clampU = material.clampV = sam;
         material.hasTextureAlpha = pin != nullptr && !pin->IsOpaque();
         body.transparent = mat->m_bOpacityActive;
      }
      m_renderer->RenderMesh(renderMask, body, material);
   }
   else
   {
      if (!m_meshBuffer || m_dynamicVertexBufferRegenerate)
      {
         GenerateVertexBuffer();
         m_meshEdgeBuffer = nullptr; // Edge mesh shares the previous vertex buffer
      }

      if (!m_meshBuffer)
         return; // Nothing to draw on a degenerate curve

      const uint32_t floorIndexCount = (m_rampVertex - 1) * 6;
      const bool hasWalls = m_d.m_leftwallheightvisible != 0.f || m_d.m_rightwallheightvisible != 0.f;

      RenderMeshDesc floor;
      floor.mesh = m_meshBuffer;
      floor.indexCount = floorIndexCount;
      floor.sortCenter = m_boundingSphereCenter;
      floor.depthBias = m_d.m_depthBias;
      floor.uiEdgeMesh = &m_meshEdgeBuffer;
      floor.edgeMode = UIEdgeMode::Quads;
      floor.edgeTriIndices = m_meshIndices.data();
      floor.edgeTriCount = hasWalls ? m_meshIndices.size() : floorIndexCount;
      floor.edgeTriIndices16 = true;

      RenderMeshDesc rightWall = floor;
      rightWall.startIndex = floorIndexCount;
      rightWall.uiEdgeMesh = nullptr;
      rightWall.edgeMode = UIEdgeMode::None;

      RenderMeshDesc leftWall = rightWall;
      leftWall.startIndex = floorIndexCount * 2;

      VisualMaterial material;
      VisualMaterial wallsMaterial;
      Texture *const pin = isUIPass ? nullptr : m_ptable->GetImage(m_d.m_szImage);
      if (!isUIPass)
      {
         /* TODO: This is a misnomer right now, but clamp fixes some visual glitches (single-pixel lines)
          * with transparent textures. Probably the option should simply be renamed to ImageModeClamp,
          * since the texture coordinates always stay within [0,1] anyway. */
         const SamplerAddressMode sam = m_d.m_imagealignment == ImageModeWrap ? SamplerAddressMode::SA_CLAMP : SamplerAddressMode::SA_REPEAT;
         m_renderer->m_renderDevice->ResetRenderState();
         m_renderer->m_renderDevice->SetRenderState(RenderState::CULLMODE, RenderState::CULL_NONE); // as both floor and walls are thinwalled
         material.material = mat;
         material.image = pin;
         material.filter = SamplerFilter::SF_TRILINEAR;
         material.clampU = material.clampV = sam;
         material.hasTextureAlpha = pin != nullptr && !pin->IsOpaque();
         wallsMaterial = material;
         if (pin != nullptr && !m_d.m_imageWalls)
            wallsMaterial.technique = ShaderTechnique::basic_without_texture;
         floor.transparent = rightWall.transparent = leftWall.transparent = mat->m_bOpacityActive;
      }

      if ((isUIPass && hasWalls) || (m_d.m_rightwallheightvisible != 0.f && m_d.m_leftwallheightvisible != 0.f && (!pin || m_d.m_imageWalls)))
      {
         // both walls with image and floor
         m_renderer->RenderMesh(renderMask, floor, material);
         m_renderer->RenderMesh(renderMask, rightWall, wallsMaterial);
         m_renderer->RenderMesh(renderMask, leftWall, wallsMaterial);
      }
      else
      {
         // only floor
         m_renderer->RenderMesh(renderMask, floor, material);

         if (m_d.m_rightwallheightvisible != 0.f || m_d.m_leftwallheightvisible != 0.f)
         {
            if (m_d.m_rightwallheightvisible != 0.f)
               m_renderer->RenderMesh(renderMask, rightWall, wallsMaterial);
            if (m_d.m_leftwallheightvisible != 0.f)
               m_renderer->RenderMesh(renderMask, leftWall, wallsMaterial);
         }
      }
   }
}

float Ramp::GetDepth(const Vertex3Ds& viewDir) const
{
   // TODO: improve
   const Vertex2D center2D = GetCenter();
   const float centerZ = 0.5f * (m_d.m_heightbottom + m_d.m_heighttop);
   return m_d.m_depthBias + viewDir.x * center2D.x + viewDir.y * center2D.y + viewDir.z * centerZ;
}

void Ramp::UpdateBounds()
{
   const Vertex2D& center2D = m_curve.GetCenter();
   m_boundingSphereCenter.Set(center2D.x, center2D.y, 0.5f * (m_d.m_heightbottom + m_d.m_heighttop));
}

// Ported at: VisualPinball.Engine/VPT/Ramp/RampHitGenerator.cs

bool Ramp::IsHabitrail() const
{
   return m_d.m_type == RampType4Wire
       || m_d.m_type == RampType1Wire
       || m_d.m_type == RampType2Wire
       || m_d.m_type == RampType3WireLeft
       || m_d.m_type == RampType3WireRight;
}

void Ramp::CreateWire(const int numRings, const int numSegments, const Vertex2D * const midPoints, Vertex3D_NoTex2 * const rgvbuf) const
{
   Vertex3Ds prevB;
   for (int i = 0, index = 0; i < numRings; i++)
   {
      const int i2 = (i == (numRings - 1)) ? i : i + 1;
      const float height = m_rgheightInit[i];

      Vertex3Ds tangent(midPoints[i2].x - midPoints[i].x, midPoints[i2].y - midPoints[i].y, m_rgheightInit[i2] - m_rgheightInit[i]);
      if (i == numRings - 1)
      {
         // for the last spline point use the previous tangent again, otherwise we won't see the complete wire (it stops one control point too early)
         tangent.x = midPoints[i].x - midPoints[i - 1].x;
         tangent.y = midPoints[i].y - midPoints[i - 1].y;
      }
	  Vertex3Ds binorm;
	  Vertex3Ds normal;
	  if (i == 0)
      {
         Vertex3Ds up(midPoints[i2].x + midPoints[i].x, midPoints[i2].y + midPoints[i].y, m_rgheightInit[i2] - height);
         normal = CrossProduct(tangent, up);     //normal
         binorm = CrossProduct(tangent, normal);
      }
      else
      {
         normal = CrossProduct(prevB, tangent);
         binorm = CrossProduct(tangent, normal);
      }
      binorm.Normalize();
      normal.Normalize();
      prevB = binorm;

      const float inv_numRings = 1.0f / (float)numRings;
      const float inv_numSegments = 1.0f / (float)numSegments;
	  const float u = (float)i*inv_numRings;
	  for (int j = 0; j < numSegments; j++, index++)
      {
         const float v = ((float)j + u)*inv_numSegments;
         const Vertex3Ds tmp = GetRotatedAxis((float)j*(360.0f*inv_numSegments), tangent, normal) * ((float)m_d.m_wireDiameter*0.5f);
         rgvbuf[index].x = midPoints[i].x + tmp.x;
         rgvbuf[index].y = midPoints[i].y + tmp.y;
         rgvbuf[index].z = height + tmp.z;
         //texel
         rgvbuf[index].tu = u;
         rgvbuf[index].tv = v;
         const Vertex3Ds n(rgvbuf[index].x - midPoints[i].x, rgvbuf[index].y - midPoints[i].y, rgvbuf[index].z - height);
         const float len = 1.0f / sqrtf(n.x*n.x + n.y*n.y + n.z*n.z);
         rgvbuf[index].nx = n.x*len;
         rgvbuf[index].ny = n.y*len;
         rgvbuf[index].nz = n.z*len;
      }
   }
}

void Ramp::GenerateWireMesh(Vertex3D_NoTex2 **meshBuf1, Vertex3D_NoTex2 **meshBuf2)
{
   Vertex2D * middlePoints = 0;
   int accuracy;
   if (m_ptable->GetDetailLevel() < 5)
   {
      accuracy = 6;
   }
   else if (m_ptable->GetDetailLevel() >= 5 && m_ptable->GetDetailLevel() < 8)
   {
      accuracy = 8;
   }
   else
   {
      accuracy = (int)((float)m_ptable->GetDetailLevel() * 1.30000007152557373046875f); // see below
   }

   // as solid ramps are rendered into the static buffer, always use maximum precision
   const Material * const mat = m_ptable->GetMaterial(m_d.m_szMaterial);
   if (!mat->m_bOpacityActive)
      accuracy = (int)(10.f * 1.30000007152557373046875f); // see above

   delete [] m_rgheightInit;
   m_rgheightInit = nullptr;

   int splinePoints;
   const Vertex2D * const rgvLocal = GetRampVertex(splinePoints, &m_rgheightInit, nullptr, nullptr, (m_d.m_type != RampType1Wire) ? nullptr : &middlePoints, -1, false);

   const int numRings = splinePoints;
   const int numSegments = accuracy;
   if (numRings < 2)
   {
      // a wire needs at least 2 rings to be drawn, emit an empty mesh otherwise
      m_numVertices = 0;
      m_numIndices = 0;
      m_meshIndices.clear();
      delete[] rgvLocal;
      delete[] middlePoints;
      return;
   }
   m_numVertices = numRings*numSegments;
   m_numIndices = 6 * ((numRings-1)*numSegments); //m_numVertices*2+2;

   if (*meshBuf1 == nullptr)
      *meshBuf1 = new Vertex3D_NoTex2[m_numVertices];

   Vertex2D *tmpPoints = new Vertex2D[splinePoints];
   for (int i = 0; i < splinePoints; i++)
      tmpPoints[i] = rgvLocal[splinePoints * 2 - i - 1];

   m_vertBuffer = new Vertex3D_NoTex2[m_numVertices];
   m_vertBuffer2 = nullptr;
   m_meshIndices.resize(m_numIndices);

   if (m_d.m_type != RampType1Wire)
   {
      m_vertBuffer2 = new Vertex3D_NoTex2[m_numVertices];
      if (*meshBuf2 == nullptr)
         *meshBuf2 = new Vertex3D_NoTex2[m_numVertices];
   }

   if (m_d.m_type != RampType1Wire)
   {
      CreateWire(numRings, numSegments, rgvLocal, m_vertBuffer);
      CreateWire(numRings, numSegments, tmpPoints, m_vertBuffer2);
   }
   else
      CreateWire(numRings, numSegments, middlePoints, m_vertBuffer);

   // calculate faces
   for (int i = 0; i < numRings - 1; i++)
      for (int j = 0; j < numSegments; j++)
      {
         int quad[4];
         quad[0] = i*numSegments + j;

         quad[1] = i*numSegments;
         if (j != numSegments - 1)
            quad[1] += j + 1;

         if (i != numRings - 1)
         {
            quad[2] = (i + 1)*numSegments + j;
            quad[3] = (i + 1)*numSegments;
            if (j != numSegments - 1)
               quad[3] += j + 1;
         }
         else
         {
            quad[2] = j;
            if (j != numSegments - 1)
               quad[3] = j + 1;
            else
               quad[3] = 0;
         }

         const unsigned int offs = (i*numSegments + j) * 6;
         m_meshIndices[offs    ] = quad[0];
         m_meshIndices[offs + 1] = quad[1];
         m_meshIndices[offs + 2] = quad[2];
         m_meshIndices[offs + 3] = quad[3];
         m_meshIndices[offs + 4] = quad[2];
         m_meshIndices[offs + 5] = quad[1];
      }

   memcpy(*meshBuf1, m_vertBuffer, sizeof(Vertex3D_NoTex2)*m_numVertices);

   if (m_d.m_type != RampType1Wire)
   {
      memcpy(*meshBuf2, m_vertBuffer2, sizeof(Vertex3D_NoTex2)*m_numVertices);
   }

   // not necessary to reorder
   /*WORD* const tmp = reorderForsyth(m_meshIndices, m_numVertices);
   if (tmp != nullptr)
   {
   memcpy(m_meshIndices.data(), tmp, m_meshIndices.size()*sizeof(WORD));
   delete[] tmp;
   }*/

   delete[] rgvLocal;
   delete[] middlePoints;
   delete[] tmpPoints;
}

void Ramp::PrepareHabitrail()
{
   m_meshBuffer = nullptr;
   m_meshEdgeBuffer = nullptr;
   m_dynamicVertexBufferRegenerate = false;
   Vertex3D_NoTex2 *tmpBuf1 = nullptr;
   Vertex3D_NoTex2 *tmpBuf2 = nullptr;
   GenerateWireMesh(&tmpBuf1, &tmpBuf2);
   if (m_numIndices == 0) // Degenerate curve: no mesh buffer (buffers can not be empty)
   {
      delete[] tmpBuf1;
      delete[] tmpBuf2;
      return;
   }

   switch (m_d.m_type)
   {
   case RampType1Wire:
   {
      std::shared_ptr<IndexBuffer> dynamicIndexBuffer = std::make_shared<IndexBuffer>(m_renderer->m_renderDevice, m_meshIndices);
      std::shared_ptr<VertexBuffer> dynamicVertexBuffer = std::make_shared<VertexBuffer>(m_renderer->m_renderDevice, m_numVertices, (float *)tmpBuf1);
      m_meshBuffer = std::make_shared<MeshBuffer>(GetName(), dynamicVertexBuffer, dynamicIndexBuffer, true);
      break;
   }
   case RampType2Wire:
   {
      Vertex3D_NoTex2* vertices = new Vertex3D_NoTex2[m_numVertices * 2];
      memcpy(vertices, tmpBuf1, m_numVertices * sizeof(Vertex3D_NoTex2));
      memcpy(vertices + m_numVertices, tmpBuf2, m_numVertices * sizeof(Vertex3D_NoTex2));
      for (int i = 0; i < m_numVertices; i++)
      {
         // raise the wire a bit because the ball runs on a flat ramp physically
         vertices[i].z += 3.0f;
         vertices[i + m_numVertices].z += 3.0f;
      }
      WORD *indices = new WORD[m_numIndices * 2];
      memcpy(indices, m_meshIndices.data(), m_numIndices * sizeof(WORD));
      for (int i = 0; i < m_numIndices; i++)
         indices[m_numIndices + i] = indices[i] + m_numVertices;
      std::shared_ptr<IndexBuffer> dynamicIndexBuffer = std::make_shared<IndexBuffer>(m_renderer->m_renderDevice, m_numIndices * 2, indices);
      std::shared_ptr<VertexBuffer> dynamicVertexBuffer = std::make_shared<VertexBuffer>(m_renderer->m_renderDevice, m_numVertices * 2, (float *)vertices);
      m_meshBuffer = std::make_shared<MeshBuffer>(GetName(), dynamicVertexBuffer, dynamicIndexBuffer, true);
      m_numVertices *= 2;
      m_numIndices *= 2;
      delete[] vertices;
      delete[] indices;
      break;
   }
   case RampType3WireLeft:
   case RampType3WireRight:
   {
      Vertex3D_NoTex2 *vertices = new Vertex3D_NoTex2[m_numVertices * 3];
      memcpy(vertices, tmpBuf1, m_numVertices * sizeof(Vertex3D_NoTex2));
      memcpy(vertices + m_numVertices, tmpBuf2, m_numVertices * sizeof(Vertex3D_NoTex2));
      memcpy(vertices + m_numVertices*2, (m_d.m_type == RampType3WireLeft) ? tmpBuf2 : tmpBuf1, m_numVertices * sizeof(Vertex3D_NoTex2));
      for (int i = 0; i < m_numVertices; i++)
      {
         // raise the wire a bit because the ball runs on a flat ramp physically
         vertices[i                    ].z += 3.0f;
         vertices[i + m_numVertices    ].z += 3.0f;
         vertices[i + m_numVertices * 2].z += m_d.m_wireDistanceY * 0.5f;
      }
      WORD *indices = new WORD[m_numIndices * 3];
      memcpy(indices, m_meshIndices.data(), m_numIndices * sizeof(WORD));
      for (int i = 0; i < m_numIndices; i++)
      {
         indices[m_numIndices + i] = indices[i] + m_numVertices;
         indices[m_numIndices * 2 + i] = indices[i] + m_numVertices * 2;
      }
      std::shared_ptr<IndexBuffer> dynamicIndexBuffer = std::make_shared<IndexBuffer>(m_renderer->m_renderDevice, m_numIndices * 3, indices);
      std::shared_ptr<VertexBuffer> dynamicVertexBuffer = std::make_shared<VertexBuffer>(m_renderer->m_renderDevice, m_numVertices * 3, (float *)vertices);
      m_meshBuffer = std::make_shared<MeshBuffer>(GetName(), dynamicVertexBuffer, dynamicIndexBuffer, true);
      m_numVertices *= 3;
      m_numIndices *= 3;
      delete[] vertices;
      delete[] indices;
      break;
   }
   case RampType4Wire:
   {
      Vertex3D_NoTex2 *vertices = new Vertex3D_NoTex2[m_numVertices * 4];
      memcpy(vertices, tmpBuf1, m_numVertices * sizeof(Vertex3D_NoTex2));
      memcpy(vertices + m_numVertices, tmpBuf2, m_numVertices * sizeof(Vertex3D_NoTex2));
      memcpy(vertices + m_numVertices*2, tmpBuf1, m_numVertices * sizeof(Vertex3D_NoTex2));
      memcpy(vertices + m_numVertices*3, tmpBuf2, m_numVertices * sizeof(Vertex3D_NoTex2));
      for (int i = 0; i < m_numVertices; i++)
      {
         // raise the wire a bit because the ball runs on a flat ramp physically
         vertices[i                    ].z += 3.0f;
         vertices[i + m_numVertices    ].z += 3.0f;
         // upper wires
         vertices[i + m_numVertices * 2].z += m_d.m_wireDistanceY * 0.5f;
         vertices[i + m_numVertices * 3].z += m_d.m_wireDistanceY * 0.5f;
      }
      WORD *indices = new WORD[m_numIndices * 4];
      memcpy(indices, m_meshIndices.data(), m_numIndices * sizeof(WORD));
      for (int i = 0; i < m_numIndices; i++)
      {
         indices[m_numIndices + i] = indices[i] + m_numVertices;
         indices[m_numIndices*2 + i] = indices[i] + m_numVertices*2;
         indices[m_numIndices*3 + i] = indices[i] + m_numVertices*3;
      }
      std::shared_ptr<IndexBuffer> dynamicIndexBuffer = std::make_shared<IndexBuffer>(m_renderer->m_renderDevice, m_numIndices * 4, indices);
      std::shared_ptr<VertexBuffer> dynamicVertexBuffer = std::make_shared<VertexBuffer>(m_renderer->m_renderDevice, m_numVertices * 4, (float *)vertices);
      m_meshBuffer = std::make_shared<MeshBuffer>(GetName(), dynamicVertexBuffer, dynamicIndexBuffer, true);
      m_numVertices *= 4;
      m_numIndices *= 4;
      delete[] vertices;
      delete[] indices;
      break;
   }
   default:
      assert(!"unknown ramp type");
      break;
   }

   delete[] m_vertBuffer;
   m_vertBuffer = nullptr;
   delete[] tmpBuf1;
   if (m_d.m_type != RampType1Wire)
   {
      delete[] m_vertBuffer2;
      m_vertBuffer2 = nullptr;
      delete[] tmpBuf2;
   }

   // Keep the final (wire duplicated) triangle list for UI edge rendering (each copy is offset by one base vertex set)
   const unsigned int baseIndexCount = (unsigned int)m_meshIndices.size();
   if (m_numIndices != (int)baseIndexCount)
   {
      const int copies = m_numIndices / (int)baseIndexCount;
      vector<WORD> wireIndices(m_numIndices);
      for (int c = 0; c < copies; c++)
         for (unsigned int i = 0; i < baseIndexCount; i++)
            wireIndices[c * baseIndexCount + i] = m_meshIndices[i] + (WORD)(c * (m_numVertices / copies));
      m_meshIndices = std::move(wireIndices);
   }
}

#pragma endregion


void Ramp::ClearForOverwrite() { m_curve.ClearPoints(); }

void Ramp::Save(IObjectWriter& writer, const bool saveForUndo)
{
   writer.WriteFloat(FID(HTBT), m_d.m_heightbottom);
   writer.WriteFloat(FID(HTTP), m_d.m_heighttop);
   writer.WriteFloat(FID(WDBT), m_d.m_widthbottom);
   writer.WriteFloat(FID(WDTP), m_d.m_widthtop);
   writer.WriteString(FID(MATR), m_d.m_szMaterial);
   writer.WriteBool(FID(TMON), m_timerEnabled);
   writer.WriteInt(FID(TMIN), m_timerInterval);
   writer.WriteInt(FID(TYPE), m_d.m_type);
   writer.WriteWideString(FID(NAME), MakeWString(m_name));
   writer.WriteString(FID(IMAG), m_d.m_szImage);
   writer.WriteInt(FID(ALGN), m_d.m_imagealignment);
   writer.WriteBool(FID(IMGW), m_d.m_imageWalls);
   writer.WriteFloat(FID(WLHL), m_d.m_leftwallheight);
   writer.WriteFloat(FID(WLHR), m_d.m_rightwallheight);
   writer.WriteFloat(FID(WVHL), m_d.m_leftwallheightvisible);
   writer.WriteFloat(FID(WVHR), m_d.m_rightwallheightvisible);
   writer.WriteBool(FID(HTEV), m_d.m_hitEvent);
   writer.WriteFloat(FID(THRS), m_d.m_threshold);
   writer.WriteFloat(FID(ELAS), m_d.m_elasticity);
   writer.WriteFloat(FID(RFCT), m_d.m_friction);
   writer.WriteFloat(FID(RSCT), m_d.m_scatter);
   writer.WriteBool(FID(CLDR), m_d.m_collidable);
   writer.WriteBool(FID(RVIS), m_d.m_visible);
   writer.WriteFloat(FID(RADB), m_d.m_depthBias);
   writer.WriteFloat(FID(RADI), m_d.m_wireDiameter);
   writer.WriteFloat(FID(RADX), m_d.m_wireDistanceX);
   writer.WriteFloat(FID(RADY), m_d.m_wireDistanceY);
   writer.WriteBool(FID(REEN), m_d.m_reflectionEnabled);
   writer.WriteString(FID(MAPH), m_d.m_szPhysicsMaterial);
   writer.WriteBool(FID(OVPH), m_d.m_overwritePhysics);
   SaveSharedEditableFields(writer);
   m_curve.SavePoints(writer);
   // Sections: one skippable record each, written only when present, so a ramp without sections saves the same bytes as before
   for (const RampSection &section : m_d.m_sections)
   {
      writer.BeginObject(FID(RSEC), true, true);
      writer.WriteInt(FID(STYP), section.m_kind);
      writer.WriteInt(FID(SVER), RampSection::kVersion);
      writer.WriteInt(FID(SAN0), section.m_first);
      writer.WriteInt(FID(SAN1), section.m_last);
      if (section.m_kind == RampSection::EntryFlap)
      {
         writer.WriteFloat(FID(FAHD), section.m_flapAhead);
         writer.WriteFloat(FID(FLUP), section.m_flapUp);
         writer.WriteFloat(FID(FBOW), section.m_flapBow);
         writer.WriteFloat(FID(FBPS), section.m_flapBowPos);
      }
      else if (section.m_kind == RampSection::Hairpin)
      {
         writer.WriteFloat(FID(HRAD), section.m_radius);
         writer.WriteInt(FID(HOUT), section.m_outerFaces);
         writer.WriteInt(FID(HINN), section.m_innerFaces);
      }
      writer.EndObject();
   }
   writer.EndObject();
}

void Ramp::Load(IObjectReader& reader)
{
   SetDefaults(false);
   m_d.m_sections.clear();
   reader.AsObject(
      [this](int tag, IObjectReader& reader)
      {
         switch (tag)
         {
         case FID(PIID): reader.AsInt(); break;
         case FID(HTBT): m_d.m_heightbottom = reader.AsFloat(); break;
         case FID(HTTP): m_d.m_heighttop = reader.AsFloat(); break;
         case FID(WDBT): m_d.m_widthbottom = reader.AsFloat(); break;
         case FID(WDTP): m_d.m_widthtop = reader.AsFloat(); break;
         case FID(MATR): m_d.m_szMaterial = reader.AsString(); break;
         case FID(TMON): m_timerEnabled = reader.AsBool(); break;
         case FID(TMIN): m_timerInterval = reader.AsInt(); break;
         case FID(TYPE): m_d.m_type = static_cast<RampType>(reader.AsInt()); break;
         case FID(IMAG): m_d.m_szImage = reader.AsString(); break;
         case FID(ALGN): m_d.m_imagealignment = static_cast<RampImageAlignment>(reader.AsInt()); break;
         case FID(IMGW): m_d.m_imageWalls = reader.AsBool(); break;
         case FID(NAME): m_name = MakeString(reader.AsWideString()); break;
         case FID(WLHL): m_d.m_leftwallheight = reader.AsFloat(); break;
         case FID(WLHR): m_d.m_rightwallheight = reader.AsFloat(); break;
         case FID(WVHL): m_d.m_leftwallheightvisible = reader.AsFloat(); break;
         case FID(WVHR): m_d.m_rightwallheightvisible = reader.AsFloat(); break;
         case FID(HTEV): m_d.m_hitEvent = reader.AsBool(); break;
         case FID(THRS): m_d.m_threshold = reader.AsFloat(); break;
         case FID(ELAS): m_d.m_elasticity = reader.AsFloat(); break;
         case FID(RFCT): m_d.m_friction = reader.AsFloat(); break;
         case FID(RSCT): m_d.m_scatter = reader.AsFloat(); break;
         case FID(CLDR): m_d.m_collidable = reader.AsBool(); break;
         case FID(RVIS): m_d.m_visible = reader.AsBool(); break;
         case FID(REEN): m_d.m_reflectionEnabled = reader.AsBool(); break;
         case FID(RADB): m_d.m_depthBias = reader.AsFloat(); break;
         case FID(RADI): m_d.m_wireDiameter = reader.AsFloat(); break;
         case FID(RADX): m_d.m_wireDistanceX = reader.AsFloat(); break;
         case FID(RADY): m_d.m_wireDistanceY = reader.AsFloat(); break;
         case FID(MAPH): m_d.m_szPhysicsMaterial = reader.AsString(); break;
         case FID(OVPH): m_d.m_overwritePhysics = reader.AsBool(); break;
         case FID(PNTS): break; // Empty tag placed before drag point data (unused)
         case FID(DPNT): m_curve.LoadPointToken(reader); break;
         case FID(RSEC):
         {
            RampSection section;
            int version = 0;
            reader.AsObject(
               [&section, &version](int sectionTag, IObjectReader &sectionReader)
               {
                  switch (sectionTag)
                  {
                  case FID(STYP): section.m_kind = sectionReader.AsInt(); break;
                  case FID(SVER): version = sectionReader.AsInt(); break;
                  case FID(SAN0): section.m_first = sectionReader.AsInt(); break;
                  case FID(SAN1): section.m_last = sectionReader.AsInt(); break;
                  case FID(FAHD): section.m_flapAhead = max(0.f, sectionReader.AsFloat()); break;
                  case FID(FLUP): section.m_flapUp = max(0.f, sectionReader.AsFloat()); break;
                  case FID(FBOW): section.m_flapBow = max(0.f, sectionReader.AsFloat()); break;
                  case FID(FBPS): section.m_flapBowPos = clamp(sectionReader.AsFloat(), 0.f, 1.f); break;
                  case FID(HRAD): section.m_radius = max(0.f, sectionReader.AsFloat()); break;
                  case FID(HOUT): section.m_outerFaces = clamp(sectionReader.AsInt(), 1, 256); break;
                  case FID(HINN): section.m_innerFaces = clamp(sectionReader.AsInt(), 1, 256); break;
                  }
                  return true;
               },
               true);
            // An unknown kind or a newer version of a kind is dropped: the ramp keeps its plain geometry over that stretch
            if ((section.m_kind == RampSection::EntryFlap || section.m_kind == RampSection::Hairpin) && version >= 1 && version <= RampSection::kVersion)
               m_d.m_sections.push_back(section);
            else
               PLOGW << "Ramp " << m_name << ": unsupported section (kind " << section.m_kind << ", version " << version << ") ignored";
            break;
         }
         default: LoadSharedEditableField(tag, reader); break;
         }
         return true;
      });
}

void Ramp::AddPoint(const Vertex2D &v, const bool smooth)
{
   vector<RenderVertex3D> vvertex;
   GetCentralCurve(vvertex);

   int iSeg;
   Vertex2D vOut;
   ClosestPointOnPolygon(vvertex, v, vOut, iSeg, false);

   if (iSeg == -1)
   {
      // the point does not project on any curve segment (e.g. empty curve): append it at the end
      const float z = m_curve.GetPoints().empty() ? m_d.m_heightbottom : m_curve.GetPoints().back()->GetZ();
      m_curve.InsertPoint(m_curve.GetPoints().size(), std::make_unique<DragPoint>(&m_curve, v.x, v.y, z, smooth));
      m_curve.OnPointsModified();
      return;
   }

   // Go through vertices (including iSeg itself) counting control points until iSeg
   int icp = 0;
   for (int i = 0; i < (iSeg + 1); i++)
      if (vvertex[i].controlPoint)
         icp++;

   //if (icp == 0) // need to add point after the last point
   //icp = m_curve.GetPoints().size();

   // Ramps are usually always smooth; push the second point forward, and replace it with this one. Should work when index2 wraps.
   m_curve.InsertPoint(icp, std::make_unique<DragPoint>(&m_curve, vOut.x, vOut.y, (vvertex[max(iSeg - 1, 0)].z + vvertex[min(iSeg + 1, (int)vvertex.size() - 1)].z) * 0.5f, smooth));
   m_curve.OnPointsModified();
}

// FlipPointX/Y reverse the point order: hairpin ranges follow their points
static void ReverseSectionAnchors(vector<RampSection> &sections, const int npts)
{
   for (RampSection &s : sections)
      if (s.m_kind != RampSection::EntryFlap)
      {
         const int first = npts - 1 - s.m_last;
         s.m_last = npts - 1 - s.m_first;
         s.m_first = first;
      }
}

void Ramp::FlipY(const Vertex2D& pvCenter)
{
   m_curve.FlipPointY(pvCenter);
   ReverseSectionAnchors(m_d.m_sections, (int)m_curve.GetPoints().size());
}

void Ramp::FlipX(const Vertex2D& pvCenter)
{
   m_curve.FlipPointX(pvCenter);
   ReverseSectionAnchors(m_d.m_sections, (int)m_curve.GetPoints().size());
}

void Ramp::Rotate(const float ang, const Vertex2D &center, const bool useElementCenter) { m_curve.RotatePoints(ang, useElementCenter ? GetCenter() : center); }

void Ramp::Scale(const float scalex, const float scaley, const Vertex2D &center, const bool useElementCenter)
{
   m_curve.ScalePoints(scalex, scaley, useElementCenter ? GetCenter() : center);
}

void Ramp::Translate(const Vertex2D &offset) { m_curve.TranslatePoints(offset); }

STDMETHODIMP Ramp::InterfaceSupportsErrorInfo(REFIID riid)
{
   static const IID* arr[] =
   {
      &IID_IRamp
   };

   for (size_t i = 0; i < std::size(arr); i++)
      if (InlineIsEqualGUID(*arr[i], riid))
         return S_OK;

   return S_FALSE;
}

STDMETHODIMP Ramp::get_HeightBottom(float *pVal)
{
   *pVal = m_d.m_heightbottom;
   return S_OK;
}

STDMETHODIMP Ramp::put_HeightBottom(float newVal)
{
   if (m_d.m_heightbottom != newVal)
   {
      m_d.m_heightbottom = newVal;
      m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Ramp::get_HeightTop(float *pVal)
{
   *pVal = m_d.m_heighttop;
   return S_OK;
}

STDMETHODIMP Ramp::put_HeightTop(float newVal)
{
   if (m_d.m_heighttop != newVal)
   {
      m_d.m_heighttop = newVal;
      m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Ramp::get_WidthBottom(float *pVal)
{
   *pVal = m_d.m_widthbottom;
   return S_OK;
}

STDMETHODIMP Ramp::put_WidthBottom(float newVal)
{
   if (m_d.m_widthbottom != newVal)
   {
      m_d.m_widthbottom = newVal;
      m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Ramp::get_WidthTop(float *pVal)
{
   *pVal = m_d.m_widthtop;
   return S_OK;
}

STDMETHODIMP Ramp::put_WidthTop(float newVal)
{
   if (m_d.m_widthtop != newVal)
   {
      m_d.m_widthtop = newVal;
      m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Ramp::get_Material(BSTR *pVal)
{
   *pVal = MakeWideBSTR(m_d.m_szMaterial);
   return S_OK;
}

STDMETHODIMP Ramp::put_Material(BSTR newVal)
{
   m_d.m_szMaterial = MakeString(newVal);
   return S_OK;
}

STDMETHODIMP Ramp::get_Type(RampType *pVal)
{
   *pVal = m_d.m_type;
   return S_OK;
}

STDMETHODIMP Ramp::put_Type(RampType newVal)
{
   if(m_d.m_type != newVal)
   {
      m_d.m_type = newVal;
      m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Ramp::get_Image(BSTR *pVal)
{
   *pVal = MakeWideBSTR(m_d.m_szImage);
   return S_OK;
}

STDMETHODIMP Ramp::put_Image(BSTR newVal)
{
   const string szImage = MakeString(newVal);
   const Texture * const tex = m_ptable->GetImage(szImage);
   if (tex && tex->IsHDR())
   {
       ShowError("Cannot use a HDR image (.exr/.hdr) here");
       return E_FAIL;
   }

   if(!StrCompareNoCase(szImage, m_d.m_szImage))
   {
      m_d.m_szImage = szImage;
      m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Ramp::get_ImageAlignment(RampImageAlignment *pVal)
{
   *pVal = m_d.m_imagealignment;
   return S_OK;
}

STDMETHODIMP Ramp::put_ImageAlignment(RampImageAlignment newVal)
{
   if (m_d.m_imagealignment != newVal)
   {
      m_d.m_imagealignment = newVal;
      m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Ramp::get_HasWallImage(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_imageWalls);
   return S_OK;
}

STDMETHODIMP Ramp::put_HasWallImage(VARIANT_BOOL newVal)
{
   if (m_d.m_imageWalls != VBTOb(newVal))
   {
      m_d.m_imageWalls = VBTOb(newVal);
      m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Ramp::get_LeftWallHeight(float *pVal)
{
   *pVal = m_d.m_leftwallheight;
   return S_OK;
}

STDMETHODIMP Ramp::put_LeftWallHeight(float newVal)
{
   const float nv = newVal < 0.f ? 0.f : newVal;

   if (m_d.m_leftwallheight != nv)
   {
      m_d.m_leftwallheight = nv;
      m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Ramp::get_RightWallHeight(float *pVal)
{
   *pVal = m_d.m_rightwallheight;
   return S_OK;
}

STDMETHODIMP Ramp::put_RightWallHeight(float newVal)
{
   const float nv = newVal < 0.f ? 0.f : newVal;

   if (m_d.m_rightwallheight != nv)
   {
      m_d.m_rightwallheight = nv;
      m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Ramp::get_VisibleLeftWallHeight(float *pVal)
{
   *pVal = m_d.m_leftwallheightvisible;
   return S_OK;
}

STDMETHODIMP Ramp::put_VisibleLeftWallHeight(float newVal)
{
   const float nv = newVal < 0.f ? 0.f : newVal;

   if (m_d.m_leftwallheightvisible != nv)
   {
      m_d.m_leftwallheightvisible = nv;
      m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Ramp::get_VisibleRightWallHeight(float *pVal)
{
   *pVal = m_d.m_rightwallheightvisible;
   return S_OK;
}

STDMETHODIMP Ramp::put_VisibleRightWallHeight(float newVal)
{
   const float nv = newVal < 0.f ? 0.f : newVal;

   if (m_d.m_rightwallheightvisible != nv)
   {
      m_d.m_rightwallheightvisible = nv;
      m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Ramp::get_Elasticity(float *pVal)
{
   *pVal = m_d.m_elasticity;
   return S_OK;
}

STDMETHODIMP Ramp::put_Elasticity(float newVal)
{
   m_d.m_elasticity = newVal;
   return S_OK;
}

STDMETHODIMP Ramp::get_Friction(float *pVal)
{
   *pVal = m_d.m_friction;
   return S_OK;
}

STDMETHODIMP Ramp::put_Friction(float newVal)
{
   newVal = max(newVal, 0.f); // Friction can not be negative, but may exceed 1
   m_d.m_friction = newVal;

   return S_OK;
}

STDMETHODIMP Ramp::get_Scatter(float *pVal)
{
   *pVal = m_d.m_scatter;
   return S_OK;
}

STDMETHODIMP Ramp::put_Scatter(float newVal)
{
   m_d.m_scatter = newVal;
   return S_OK;
}

STDMETHODIMP Ramp::get_Collidable(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_vhoCollidable.empty() ? m_d.m_collidable : m_vhoCollidable[0]->m_enabled);

   return S_OK;
}

STDMETHODIMP Ramp::put_Collidable(VARIANT_BOOL newVal)
{
   const bool val = VBTOb(newVal);
   if (m_vhoCollidable.empty())
      m_d.m_collidable = val;
   else if (m_vhoCollidable[0]->m_enabled != val)
      for (size_t i = 0; i < m_vhoCollidable.size(); i++) //!! costly
         m_vhoCollidable[i]->m_enabled = val; //copy to hit checking on entities composing the object

   return S_OK;
}

STDMETHODIMP Ramp::get_HasHitEvent(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_hitEvent);
   return S_OK;
}

STDMETHODIMP Ramp::put_HasHitEvent(VARIANT_BOOL newVal)
{
   m_d.m_hitEvent = VBTOb(newVal);
   return S_OK;
}

STDMETHODIMP Ramp::get_Threshold(float *pVal)
{
   *pVal = m_d.m_threshold;
   return S_OK;
}

STDMETHODIMP Ramp::put_Threshold(float newVal)
{
   m_d.m_threshold = newVal;
   return S_OK;
}


STDMETHODIMP Ramp::get_Visible(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_visible);
   return S_OK;
}

STDMETHODIMP Ramp::put_Visible(VARIANT_BOOL newVal)
{
   m_d.m_visible = VBTOb(newVal);
   return S_OK;
}

STDMETHODIMP Ramp::get_ReflectionEnabled(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_reflectionEnabled);
   return S_OK;
}

STDMETHODIMP Ramp::put_ReflectionEnabled(VARIANT_BOOL newVal)
{
   m_d.m_reflectionEnabled = VBTOb(newVal);
   return S_OK;
}

STDMETHODIMP Ramp::get_DepthBias(float *pVal)
{
   *pVal = m_d.m_depthBias;
   return S_OK;
}

STDMETHODIMP Ramp::put_DepthBias(float newVal)
{
   m_d.m_depthBias = newVal;
   return S_OK;
}

STDMETHODIMP Ramp::get_WireDiameter(float *pVal)
{
   *pVal = m_d.m_wireDiameter;
   return S_OK;
}

STDMETHODIMP Ramp::put_WireDiameter(float newVal)
{
   if (m_d.m_wireDiameter != newVal)
   {
       m_d.m_wireDiameter = newVal;
       m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Ramp::get_WireDistanceX(float *pVal)
{
   *pVal = m_d.m_wireDistanceX;
   return S_OK;
}

STDMETHODIMP Ramp::put_WireDistanceX(float newVal)
{
   if (m_d.m_wireDistanceX != newVal)
   {
       m_d.m_wireDistanceX = newVal;
       m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Ramp::get_WireDistanceY(float *pVal)
{
   *pVal = m_d.m_wireDistanceY;
   return S_OK;
}

STDMETHODIMP Ramp::put_WireDistanceY(float newVal)
{
   if (m_d.m_wireDistanceY != newVal)
   {
       m_d.m_wireDistanceY = newVal;
       m_dynamicVertexBufferRegenerate = true;
   }

   return S_OK;
}

STDMETHODIMP Ramp::get_PhysicsMaterial(BSTR *pVal)
{
   *pVal = MakeWideBSTR(m_d.m_szPhysicsMaterial);
   return S_OK;
}

STDMETHODIMP Ramp::put_PhysicsMaterial(BSTR newVal)
{
   m_d.m_szPhysicsMaterial = MakeString(newVal);
   return S_OK;
}

STDMETHODIMP Ramp::get_OverwritePhysics(VARIANT_BOOL *pVal)
{
   *pVal = FTOVB(m_d.m_overwritePhysics);
   return S_OK;
}

STDMETHODIMP Ramp::put_OverwritePhysics(VARIANT_BOOL newVal)
{
   m_d.m_overwritePhysics = VBTOb(newVal);
   return S_OK;
}


void Ramp::ExportMesh(ObjLoader& loader)
{
   if (m_d.m_visible)
   {
      const string& name = m_name;
      loader.WriteObjectName(name);
      if (!IsHabitrail())
      {
         Vertex3D_NoTex2 *rampMesh = nullptr;
         GenerateRampMesh(&rampMesh);
         const Material * const mat = m_ptable->GetMaterial(m_d.m_szMaterial);
         const int listLength = max(0, (m_rampVertex - 1) * 6);
         unsigned int numVers = m_numVertices * 3;
         if (m_d.m_rightwallheightvisible == 0.0f && m_d.m_leftwallheightvisible == 0.0f)
            numVers = m_numVertices;
         loader.WriteVertexInfo(rampMesh, numVers);
         loader.WriteMaterial(m_d.m_szMaterial, string(), mat);
         loader.UseTexture(m_d.m_szMaterial);

         if (m_d.m_rightwallheightvisible != 0.f && m_d.m_leftwallheightvisible != 0.f)
         {
            loader.WriteFaceInfoList(m_meshIndices.data(), listLength * 3);
            loader.UpdateFaceOffset(numVers);
         }
         else
         {
            if (m_d.m_rightwallheightvisible != 0.0f)
            {
               loader.WriteFaceInfoList(m_meshIndices.data(), listLength * 2);
               loader.UpdateFaceOffset(m_numVertices * 3);
            }
            else if (m_d.m_leftwallheightvisible != 0.0f)
            {
               loader.WriteFaceInfoList(m_meshIndices.data(), listLength);
               loader.UpdateFaceOffset(m_numVertices * 2);
               loader.WriteFaceInfoList(m_meshIndices.data(), listLength);
               loader.UpdateFaceOffset(m_numVertices);
            }
            else
            {
               loader.WriteFaceInfoList(m_meshIndices.data(), listLength);
               loader.UpdateFaceOffset(m_numVertices);
            }
         }
         delete[] rampMesh;
      }
      else
      {
         Vertex3D_NoTex2 *tmpBuf1 = nullptr, *tmpBuf2 = nullptr;
         GenerateWireMesh(&tmpBuf1, &tmpBuf2);
         const Material * const mat = m_ptable->GetMaterial(m_d.m_szMaterial);
         if (m_d.m_type == RampType1Wire)
         {
            loader.WriteVertexInfo(tmpBuf1, m_numVertices);
            loader.WriteMaterial(m_d.m_szMaterial, string(), mat);
            loader.UseTexture(m_d.m_szMaterial);
            loader.WriteFaceInfo(m_meshIndices);
            loader.UpdateFaceOffset(m_numVertices);
         }
         else if (m_d.m_type == RampType2Wire)
         {
            Vertex3D_NoTex2 * const tmp = new Vertex3D_NoTex2[m_numVertices * 2];
            memcpy(tmp, tmpBuf1, sizeof(Vertex3D_NoTex2)*m_numVertices);
            memcpy(&tmp[m_numVertices], tmpBuf2, sizeof(Vertex3D_NoTex2)*m_numVertices);
            for (int i = 0; i < m_numVertices * 2; i++)
               tmp[i].z += 3.0f;
            loader.WriteVertexInfo(tmp, m_numVertices * 2);
            delete[] tmp;
            loader.WriteFaceInfo(m_meshIndices);
            loader.WriteMaterial(m_d.m_szMaterial, string(), mat);
            loader.UseTexture(m_d.m_szMaterial);
            WORD * const idx = new WORD[m_meshIndices.size()];
            for (size_t i = 0; i < m_meshIndices.size(); i++)
               idx[i] = m_meshIndices[i] + m_numVertices;
            loader.WriteFaceInfoList(idx, (unsigned int)m_meshIndices.size());
            loader.UpdateFaceOffset(m_numVertices * 2);
            delete[] idx;
         }
         else if (m_d.m_type == RampType4Wire)
         {
            Vertex3D_NoTex2 * const tmp = new Vertex3D_NoTex2[m_numVertices * 4];
            memcpy(tmp, tmpBuf1, sizeof(Vertex3D_NoTex2)*m_numVertices);
            memcpy(&tmp[m_numVertices], tmpBuf2, sizeof(Vertex3D_NoTex2)*m_numVertices);
            memcpy(&tmp[m_numVertices * 2], tmpBuf1, sizeof(Vertex3D_NoTex2)*m_numVertices);
            memcpy(&tmp[m_numVertices * 3], tmpBuf2, sizeof(Vertex3D_NoTex2)*m_numVertices);
            for (int i = 0; i < m_numVertices * 2; i++)
               tmp[i].z += m_d.m_wireDistanceY*0.5f;

            for (int i = m_numVertices * 2; i < m_numVertices * 4; i++)
               tmp[i].z += 3.0f;
            loader.WriteVertexInfo(tmp, m_numVertices * 4);
            delete[] tmp;
            loader.WriteMaterial(m_d.m_szMaterial, string(), mat);
            loader.UseTexture(m_d.m_szMaterial);
            loader.WriteFaceInfo(m_meshIndices);
            WORD * const idx = new WORD[m_meshIndices.size()];
            for (size_t i = 0; i < m_meshIndices.size(); i++)
               idx[i] = m_meshIndices[i] + m_numVertices;
            loader.WriteFaceInfoList(idx, (unsigned int)m_meshIndices.size());
            for (size_t i = 0; i < m_meshIndices.size(); i++)
               idx[i] = m_meshIndices[i] + m_numVertices * 2;
            loader.WriteFaceInfoList(idx, (unsigned int)m_meshIndices.size());
            for (size_t i = 0; i < m_meshIndices.size(); i++)
               idx[i] = m_meshIndices[i] + m_numVertices * 3;
            loader.WriteFaceInfoList(idx, (unsigned int)m_meshIndices.size());
            loader.UpdateFaceOffset(m_numVertices * 4);
            delete[] idx;
         }
         else if (m_d.m_type == RampType3WireLeft)
         {
            Vertex3D_NoTex2 * const tmp = new Vertex3D_NoTex2[m_numVertices * 3];
            memcpy(tmp, tmpBuf2, sizeof(Vertex3D_NoTex2)*m_numVertices);
            memcpy(&tmp[m_numVertices], tmpBuf1, sizeof(Vertex3D_NoTex2)*m_numVertices);
            memcpy(&tmp[m_numVertices * 2], tmpBuf2, sizeof(Vertex3D_NoTex2)*m_numVertices);
            for (int i = 0; i < m_numVertices; i++)
               tmp[i].z += m_d.m_wireDistanceY*0.5f;

            for (int i = m_numVertices; i < m_numVertices * 3; i++)
               tmp[i].z += 3.0f;
            loader.WriteVertexInfo(tmp, m_numVertices * 3);
            delete[] tmp;
            loader.WriteMaterial(m_d.m_szMaterial, string(), mat);
            loader.UseTexture(m_d.m_szMaterial);
            loader.WriteFaceInfo(m_meshIndices);
            WORD * const idx = new WORD[m_meshIndices.size()];
            for (size_t i = 0; i < m_meshIndices.size(); i++)
               idx[i] = m_meshIndices[i] + m_numVertices;
            loader.WriteFaceInfoList(idx, (unsigned int)m_meshIndices.size());
            for (size_t i = 0; i < m_meshIndices.size(); i++)
               idx[i] = m_meshIndices[i] + m_numVertices * 2;
            loader.WriteFaceInfoList(idx, (unsigned int)m_meshIndices.size());
            loader.UpdateFaceOffset(m_numVertices * 3);
            delete[] idx;
         }
         else if (m_d.m_type == RampType3WireRight)
         {
            Vertex3D_NoTex2 * const tmp = new Vertex3D_NoTex2[m_numVertices * 3];
            memcpy(tmp, tmpBuf1, sizeof(Vertex3D_NoTex2)*m_numVertices);
            memcpy(&tmp[m_numVertices], tmpBuf1, sizeof(Vertex3D_NoTex2)*m_numVertices);
            memcpy(&tmp[m_numVertices * 2], tmpBuf2, sizeof(Vertex3D_NoTex2)*m_numVertices);
            for (int i = 0; i < m_numVertices; i++)
               tmp[i].z += m_d.m_wireDistanceY*0.5f;

            for (int i = m_numVertices; i < m_numVertices * 3; i++)
               tmp[i].z += 3.0f;
            loader.WriteVertexInfo(tmp, m_numVertices * 3);
            delete[] tmp;
            loader.WriteMaterial(m_d.m_szMaterial, string(), mat);
            loader.UseTexture(m_d.m_szMaterial);
            loader.WriteFaceInfo(m_meshIndices);
            WORD * const __restrict idx = new WORD[m_meshIndices.size()];
            for (size_t i = 0; i < m_meshIndices.size(); i++)
               idx[i] = m_meshIndices[i] + m_numVertices;
            loader.WriteFaceInfoList(idx, (unsigned int)m_meshIndices.size());
            for (size_t i = 0; i < m_meshIndices.size(); i++)
               idx[i] = m_meshIndices[i] + m_numVertices * 2;
            loader.WriteFaceInfoList(idx, (unsigned int)m_meshIndices.size());
            loader.UpdateFaceOffset(m_numVertices * 3);
            delete[] idx;
         }

         delete[] tmpBuf1;
         if (m_d.m_type != RampType1Wire)
            delete[] tmpBuf2;
      }
   }
}

// Ported at: VisualPinball.Engine/VPT/Ramp/RampMeshGenerator.cs

void Ramp::GenerateRampMesh(Vertex3D_NoTex2 **meshBuf)
{
   const Texture * const pin = m_ptable->GetImage(m_d.m_szImage);
   float *rgheight;
   float *rgratio = nullptr;
   bool *rgwall; // sectioned ramp: rows without walls get a zero height wall (they only meet walled rows on a zero length segment)
   const Vertex2D * const rgvLocal = GetRampVertex(m_rampVertex, &rgheight, nullptr, (m_d.m_imagealignment == ImageModeWorld) ? nullptr : &rgratio, nullptr, -1, true, &rgwall);

   if (m_rampVertex == 0)
   {
      // degenerate curve, emit an empty mesh
      m_numVertices = 0;
      m_numIndices = 0;
      m_meshIndices.clear();
      delete[] rgvLocal;
      delete[] rgheight;
      delete[] rgratio;
      delete[] rgwall;
      return;
   }

   const float inv_tablewidth = 1.0f / (m_ptable->m_right - m_ptable->m_left);
   const float inv_tableheight = 1.0f / (m_ptable->m_bottom - m_ptable->m_top);

   m_numVertices = m_rampVertex * 2;
   const unsigned int rgioffset = (m_rampVertex - 1) * 6;
   m_numIndices = rgioffset * 3; // to draw the full ramp in one go (could only use *1, and draw three times with offsets into vertices)

   if (*meshBuf == nullptr)
   {
      *meshBuf = new Vertex3D_NoTex2[m_numVertices * 3];
   }
   Vertex3D_NoTex2 * const buf = *meshBuf;

   m_vertBuffer = new Vertex3D_NoTex2[m_numVertices];
   m_meshIndices.resize(m_numIndices);
   for (int i = 0; i < m_rampVertex; i++)
   {
      Vertex3D_NoTex2 * const rgv3D = m_vertBuffer + i * 2;
      rgv3D[0].x = rgvLocal[i].x;
      rgv3D[0].y = rgvLocal[i].y;
      rgv3D[0].z = rgheight[i];

      rgv3D[1].x = rgvLocal[m_rampVertex * 2 - i - 1].x;
      rgv3D[1].y = rgvLocal[m_rampVertex * 2 - i - 1].y;
      rgv3D[1].z = rgv3D[0].z;

      if (pin)
      {
         if (m_d.m_imagealignment == ImageModeWorld)
         {
            rgv3D[0].tu = rgv3D[0].x * inv_tablewidth;
            rgv3D[0].tv = rgv3D[0].y * inv_tableheight;
            rgv3D[1].tu = rgv3D[1].x * inv_tablewidth;
            rgv3D[1].tv = rgv3D[1].y * inv_tableheight;
         }
         else
         {
            rgv3D[0].tu = 1.0f;
            rgv3D[0].tv = rgratio[i];
            rgv3D[1].tu = 0;
            rgv3D[1].tv = rgratio[i];
         }
      }
      else
      {
         rgv3D[0].tu = 0.0f;
         rgv3D[0].tv = 0.0f;
         rgv3D[1].tu = 0.0f;
         rgv3D[1].tv = 0.0f;
      }

      if (i == m_rampVertex - 1)
         break;

      //floor
      unsigned int offs = i * 6;
      m_meshIndices[offs] = i * 2;
      m_meshIndices[offs + 1] = i * 2 + 1;
      m_meshIndices[offs + 2] = i * 2 + 3;
      m_meshIndices[offs + 3] = i * 2;
      m_meshIndices[offs + 4] = i * 2 + 3;
      m_meshIndices[offs + 5] = i * 2 + 2;

      //walls
      offs += rgioffset;
      m_meshIndices[offs] = i * 2 + m_numVertices;
      m_meshIndices[offs + 1] = i * 2 + m_numVertices + 1;
      m_meshIndices[offs + 2] = i * 2 + m_numVertices + 3;
      m_meshIndices[offs + 3] = i * 2 + m_numVertices;
      m_meshIndices[offs + 4] = i * 2 + m_numVertices + 3;
      m_meshIndices[offs + 5] = i * 2 + m_numVertices + 2;

      offs += rgioffset;
      m_meshIndices[offs] = i * 2 + m_numVertices * 2;
      m_meshIndices[offs + 1] = i * 2 + m_numVertices * 2 + 1;
      m_meshIndices[offs + 2] = i * 2 + m_numVertices * 2 + 3;
      m_meshIndices[offs + 3] = i * 2 + m_numVertices * 2;
      m_meshIndices[offs + 4] = i * 2 + m_numVertices * 2 + 3;
      m_meshIndices[offs + 5] = i * 2 + m_numVertices * 2 + 2;
   }
   ComputeNormals(m_vertBuffer, m_numVertices, m_meshIndices.data(), (m_rampVertex - 1) * 6);

   // Flip Normals if pointing downwards instead of upwards, not necessary anymore //!! hacky, do it correct somehow else
   /*for (int i = 0; i < rampVertex; i++)
       for (int j = 0; j < 2; ++j) if (m_vertBuffer[i * 2 + j].nz < 0.0f) {
       m_vertBuffer[i * 2 + j].nx = -m_vertBuffer[i * 2 + j].nx;
       m_vertBuffer[i * 2 + j].ny = -m_vertBuffer[i * 2 + j].ny;
       m_vertBuffer[i * 2 + j].nz = -m_vertBuffer[i * 2 + j].nz;
       }*/

   unsigned int offset = 0;
   memcpy(&buf[offset], m_vertBuffer, sizeof(Vertex3D_NoTex2)*m_numVertices);
   offset += m_numVertices;

   // only calculate vertices if one or both sides are visible (!=0)
   if (m_d.m_leftwallheightvisible != 0.f || m_d.m_rightwallheightvisible != 0.f)
   {
      for (int i = 0; i < m_rampVertex; i++)
      {
         Vertex3D_NoTex2 * const rgv3D = m_vertBuffer + i * 2;

         rgv3D[1].x = rgvLocal[i].x;
         rgv3D[1].y = rgvLocal[i].y;
         rgv3D[1].z = rgheight[i] + ((rgwall == nullptr || rgwall[i]) ? m_d.m_rightwallheightvisible : 0.f);

         if (pin && m_d.m_imageWalls)
         {
            if (m_d.m_imagealignment == ImageModeWorld)
            {
               rgv3D[0].tu = rgv3D[0].x * inv_tablewidth;
               rgv3D[0].tv = rgv3D[0].y * inv_tableheight;
            }
            else
            {
               rgv3D[0].tu = 0;
               rgv3D[0].tv = rgratio[i];
            }

            rgv3D[1].tu = rgv3D[0].tu;
            rgv3D[1].tv = rgv3D[0].tv;
         }
         else
         {
            rgv3D[0].tu = 0.0f;
            rgv3D[0].tv = 0.0f;
            rgv3D[1].tu = 0.0f;
            rgv3D[1].tv = 0.0f;
         }
      }
      ComputeNormals(m_vertBuffer, m_numVertices, m_meshIndices.data(), (m_rampVertex - 1) * 6);
      memcpy(&buf[offset], m_vertBuffer, sizeof(Vertex3D_NoTex2)*m_numVertices);
      offset += m_numVertices;

      for (int i = 0; i < m_rampVertex; i++)
      {
         Vertex3D_NoTex2 * const rgv3D = m_vertBuffer + i * 2;
         rgv3D[0].x = rgvLocal[m_rampVertex * 2 - i - 1].x;
         rgv3D[0].y = rgvLocal[m_rampVertex * 2 - i - 1].y;
         rgv3D[0].z = rgheight[i];

         rgv3D[1].x = rgv3D[0].x;
         rgv3D[1].y = rgv3D[0].y;
         rgv3D[1].z = rgheight[i] + ((rgwall == nullptr || rgwall[i]) ? m_d.m_leftwallheightvisible : 0.f);

         if (pin && m_d.m_imageWalls)
         {
            if (m_d.m_imagealignment == ImageModeWorld)
            {
               rgv3D[0].tu = rgv3D[0].x * inv_tablewidth;
               rgv3D[0].tv = rgv3D[0].y * inv_tableheight;
            }
            else
            {
               rgv3D[0].tu = 0;
               rgv3D[0].tv = rgratio[i];
            }

            rgv3D[1].tu = rgv3D[0].tu;
            rgv3D[1].tv = rgv3D[0].tv;
         }
         else
         {
            rgv3D[0].tu = 0.0f;
            rgv3D[0].tv = 0.0f;
            rgv3D[1].tu = 0.0f;
            rgv3D[1].tv = 0.0f;
         }
      }
      ComputeNormals(m_vertBuffer, m_numVertices, m_meshIndices.data(), (m_rampVertex - 1) * 6);
      memcpy(&buf[offset], m_vertBuffer, sizeof(Vertex3D_NoTex2)*m_numVertices);
   }

   delete[] m_vertBuffer;
   m_vertBuffer = nullptr;
   delete[] rgvLocal;
   delete[] rgheight;
   delete[] rgratio;
   delete[] rgwall;
}

void Ramp::GenerateVertexBuffer()
{
   m_dynamicVertexBufferRegenerate = false;

   Vertex3D_NoTex2 *tmpBuffer = nullptr;
   GenerateRampMesh(&tmpBuffer);
   if (m_numIndices == 0) // Degenerate curve: no mesh buffer (buffers can not be empty)
   {
      delete[] tmpBuffer;
      m_meshBuffer = nullptr;
      m_meshEdgeBuffer = nullptr;
      return;
   }
   std::shared_ptr<VertexBuffer> dynamicVertexBuffer
      = std::make_shared<VertexBuffer>(m_renderer->m_renderDevice, m_numVertices * 3, (float *)tmpBuffer); //!! use USAGE_DYNAMIC if it would actually be "really" dynamic
   delete[] tmpBuffer;

   std::shared_ptr<IndexBuffer> dynamicIndexBuffer = std::make_shared<IndexBuffer>(m_renderer->m_renderDevice, m_meshIndices);

   m_meshBuffer = std::make_shared<MeshBuffer>(GetName(), dynamicVertexBuffer, dynamicIndexBuffer, true);
}
