#include "TchSemanticExport.h"
#include "JsonWriter.h"
#include "SqliteGuard.h"
#include "sqlite3.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace MiniCAD::SaTool
{
    namespace
    {
        constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;

        std::string TrimText(std::string s)
        {
            while (!s.empty() && (s.back() == '\0' || s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\t'))
                s.pop_back();
            size_t b = 0;
            while (b < s.size() && (s[b] == ' ' || s[b] == '\r' || s[b] == '\n' || s[b] == '\t'))
                ++b;
            return b > 0 ? s.substr(b) : s;
        }

        std::string FormatSpanText(double v)
        {
            char buf[64];
            if (std::abs(v - std::round(v)) < 1e-3)
                std::snprintf(buf, sizeof(buf), "%.0f", std::round(v));
            else
                std::snprintf(buf, sizeof(buf), "%.1f", v);
            return std::string(buf);
        }

        void MakeApproxTextBox(double x, double y, double h, const std::string& text,
                               double& minx, double& miny, double& maxx, double& maxy)
        {
            const double effH = h > 1e-3 ? h : 100.0;
            const double w = std::max(effH, effH * 0.6 * (static_cast<double>(text.size()) / 1.5));
            minx = x - w * 0.25;
            maxx = x + w * 0.75;
            miny = y - effH * 0.3;
            maxy = y + effH;
        }
    }

    bool WriteTchSemanticTables(sqlite3* db, const CadSemanticSnapshot& snap,
                                TchExportCounts& counts, std::string* error)
    {
        counts = {};

        // 1. tch_walls
        {
            ScopedSqliteStmt st;
            const char* sql =
                "INSERT INTO tch_walls("
                "handle,dxf_name,layer,space,layout_name,"
                "start_x,start_y,start_z,end_x,end_y,end_z,length,"
                "arc_angle_rad,arc_cx,arc_cy,arc_radius,"
                "left_width,right_width,total_thickness,height,"
                "material_code,material_name,flags,scale,opening_count,"
                "joins_json,has_connections,connected_wall_handles_json,"
                "minx,miny,maxx,maxy"
                ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            if (!st.Prepare(db, sql, error)) return false;
            for (const auto& w : snap.TchWalls)
            {
                if (!w.BlockName.empty()) continue;

                JsonWriter jw;
                jw.BeginArray();
                for (double j : w.Joins) jw.Double(j, 3);
                jw.EndArray();

                JsonWriter cw;
                cw.BeginArray();
                for (uint64_t ch : w.ConnectedHandles)
                    if (ch != 0) cw.String(FormatHandleHex(ch));
                cw.EndArray();

                const bool arcOk = w.HasArc
                    ? (st.BindDouble(14, w.ArcCx, error) && st.BindDouble(15, w.ArcCy, error) && st.BindDouble(16, w.ArcRadius, error))
                    : (st.BindNull(14, error) && st.BindNull(15, error) && st.BindNull(16, error));
                const bool hOk = w.HasHeight ? st.BindDouble(20, w.Height, error) : st.BindNull(20, error);

                if (!st.BindText(1, FormatHandleHex(w.Handle), error)
                    || !st.BindText(2, w.DxfName, error)
                    || !st.BindText(3, w.Layer, error)
                    || !st.BindText(4, w.Space, error)
                    || !st.BindText(5, w.LayoutName, error)
                    || !st.BindDouble(6, w.StartX, error)
                    || !st.BindDouble(7, w.StartY, error)
                    || !st.BindDouble(8, w.StartZ, error)
                    || !st.BindDouble(9, w.EndX, error)
                    || !st.BindDouble(10, w.EndY, error)
                    || !st.BindDouble(11, w.EndZ, error)
                    || !st.BindDouble(12, w.Length, error)
                    || !st.BindDouble(13, w.ArcAngleRad, error)
                    || !arcOk
                    || !st.BindDouble(17, w.LeftWidth, error)
                    || !st.BindDouble(18, w.RightWidth, error)
                    || !st.BindDouble(19, w.TotalThickness, error)
                    || !hOk
                    || !st.BindInt(21, w.MaterialCode, error)
                    || !st.BindText(22, w.MaterialName, error)
                    || !st.BindInt64(23, static_cast<int64_t>(w.Flags), error)
                    || !st.BindDouble(24, w.Scale, error)
                    || !st.BindInt(25, w.OpeningCount, error)
                    || !st.BindText(26, jw.Str(), error)
                    || !st.BindInt(27, w.HasConnections ? 1 : 0, error)
                    || !st.BindText(28, cw.Str(), error)
                    || !st.BindDouble(29, w.MinX, error)
                    || !st.BindDouble(30, w.MinY, error)
                    || !st.BindDouble(31, w.MaxX, error)
                    || !st.BindDouble(32, w.MaxY, error)
                    || !st.StepDone(error))
                {
                    return false;
                }
                ++counts.Walls;
            }
        }

        // 2. tch_openings
        {
            ScopedSqliteStmt st;
            const char* sql =
                "INSERT INTO tch_openings("
                "handle,parent_handle,layer,label_layer,space,layout_name,"
                "kind_code,kind_name,label,x,y,sill_z,width,height,thickness,"
                "angle_rad,angle_deg,mirror,mirror_x,mirror_y,wall_handle,"
                "label_x,label_y,label_height,scale,flags,display_flags,"
                "minx,miny,maxx,maxy"
                ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            if (!st.Prepare(db, sql, error)) return false;
            for (const auto& o : snap.TchOpenings)
            {
                if (!o.BlockName.empty()) continue;
                if (!st.BindText(1, FormatHandleHex(o.Handle), error)
                    || !st.BindOptText(2, FormatHandleHex(o.ParentHandle), error)
                    || !st.BindText(3, o.Layer, error)
                    || !st.BindOptText(4, o.LabelLayer, error)
                    || !st.BindText(5, o.Space, error)
                    || !st.BindText(6, o.LayoutName, error)
                    || !st.BindInt(7, o.KindCode, error)
                    || !st.BindText(8, o.KindName, error)
                    || !st.BindOptText(9, o.Label, error)
                    || !st.BindDouble(10, o.X, error)
                    || !st.BindDouble(11, o.Y, error)
                    || !st.BindDouble(12, o.SillZ, error)
                    || !st.BindDouble(13, o.Width, error)
                    || !st.BindDouble(14, o.Height, error)
                    || !st.BindDouble(15, o.Thickness, error)
                    || !st.BindDouble(16, o.AngleRad, error)
                    || !st.BindDouble(17, o.AngleRad * kRadToDeg, error)
                    || !st.BindInt(18, o.Mirror, error)
                    || !st.BindInt(19, o.MirrorX ? 1 : 0, error)
                    || !st.BindInt(20, o.MirrorY ? 1 : 0, error)
                    || !st.BindOptText(21, FormatHandleHex(o.WallHandle), error)
                    || !st.BindDouble(22, o.LabelX, error)
                    || !st.BindDouble(23, o.LabelY, error)
                    || !st.BindDouble(24, o.LabelHeight, error)
                    || !st.BindDouble(25, o.Scale, error)
                    || !st.BindInt(26, o.Flags, error)
                    || !st.BindInt(27, o.DisplayFlags, error)
                    || !st.BindDouble(28, o.MinX, error)
                    || !st.BindDouble(29, o.MinY, error)
                    || !st.BindDouble(30, o.MaxX, error)
                    || !st.BindDouble(31, o.MaxY, error)
                    || !st.StepDone(error))
                {
                    return false;
                }
                ++counts.Openings;
            }
        }

        // 3. tch_columns
        {
            ScopedSqliteStmt st;
            const char* sql =
                "INSERT INTO tch_columns("
                "handle,layer,space,layout_name,x,y,base_z,height,"
                "material_code,material_name,scale,area_m2,outline_json,"
                "minx,miny,maxx,maxy"
                ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            if (!st.Prepare(db, sql, error)) return false;
            for (const auto& c : snap.TchColumns)
            {
                if (!c.BlockName.empty()) continue;

                JsonWriter ow;
                ow.BeginArray();
                for (const auto& v : c.Outline)
                {
                    ow.BeginArray();
                    ow.Double(v.X, 3); ow.Double(v.Y, 3); ow.Double(v.Bulge, 6);
                    ow.EndArray();
                }
                ow.EndArray();

                const bool areaOk = c.HasArea ? st.BindDouble(12, c.AreaM2, error) : st.BindNull(12, error);
                if (!st.BindText(1, FormatHandleHex(c.Handle), error)
                    || !st.BindText(2, c.Layer, error)
                    || !st.BindText(3, c.Space, error)
                    || !st.BindText(4, c.LayoutName, error)
                    || !st.BindDouble(5, c.X, error)
                    || !st.BindDouble(6, c.Y, error)
                    || !st.BindDouble(7, c.BaseZ, error)
                    || !st.BindDouble(8, c.Height, error)
                    || !st.BindInt(9, c.MaterialCode, error)
                    || !st.BindText(10, c.MaterialName, error)
                    || !st.BindDouble(11, c.Scale, error)
                    || !areaOk
                    || !st.BindText(13, ow.Str(), error)
                    || !st.BindDouble(14, c.MinX, error)
                    || !st.BindDouble(15, c.MinY, error)
                    || !st.BindDouble(16, c.MaxX, error)
                    || !st.BindDouble(17, c.MaxY, error)
                    || !st.StepDone(error))
                {
                    return false;
                }
                ++counts.Columns;
            }
        }

        // 4. tch_spaces
        {
            ScopedSqliteStmt st;
            const char* sql =
                "INSERT INTO tch_spaces("
                "handle,layer,space,layout_name,name,area_m2,perimeter_mm,"
                "centroid_x,centroid_y,scale,outline_json,minx,miny,maxx,maxy"
                ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            if (!st.Prepare(db, sql, error)) return false;
            for (const auto& s : snap.TchSpaces)
            {
                if (!s.BlockName.empty()) continue;

                JsonWriter ow;
                ow.BeginArray();
                for (const auto& [px, py] : s.Outline)
                {
                    ow.BeginArray(); ow.Double(px, 3); ow.Double(py, 3); ow.EndArray();
                }
                ow.EndArray();

                if (!st.BindText(1, FormatHandleHex(s.Handle), error)
                    || !st.BindText(2, s.Layer, error)
                    || !st.BindText(3, s.Space, error)
                    || !st.BindText(4, s.LayoutName, error)
                    || !st.BindOptText(5, s.Name, error)
                    || !st.BindDouble(6, s.AreaM2, error)
                    || !st.BindDouble(7, s.PerimeterMm, error)
                    || !st.BindDouble(8, s.CentroidX, error)
                    || !st.BindDouble(9, s.CentroidY, error)
                    || !st.BindDouble(10, s.Scale, error)
                    || !st.BindText(11, ow.Str(), error)
                    || !st.BindDouble(12, s.MinX, error)
                    || !st.BindDouble(13, s.MinY, error)
                    || !st.BindDouble(14, s.MaxX, error)
                    || !st.BindDouble(15, s.MaxY, error)
                    || !st.StepDone(error))
                {
                    return false;
                }
                ++counts.Spaces;
            }
        }

        // 5. tch_stairs
        {
            ScopedSqliteStmt st;
            const char* sql =
                "INSERT INTO tch_stairs("
                "handle,stair_kind,layer,space,layout_name,x,y,rotation_deg,"
                "step_height,tread_width,flight_width,landing_width,total_width,"
                "steps1,steps2,floor_type,floor_type_name,mirror,scale,extra_json,"
                "minx,miny,maxx,maxy"
                ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            if (!st.Prepare(db, sql, error)) return false;
            for (const auto& s : snap.TchStairs)
            {
                if (!s.BlockName.empty()) continue;
                const bool mirrorOk = s.Mirror.has_value()
                    ? st.BindInt(18, *s.Mirror ? 1 : 0, error)
                    : st.BindNull(18, error);
                if (!st.BindText(1, FormatHandleHex(s.Handle), error)
                    || !st.BindText(2, s.StairKind, error)
                    || !st.BindText(3, s.Layer, error)
                    || !st.BindText(4, s.Space, error)
                    || !st.BindText(5, s.LayoutName, error)
                    || !st.BindDouble(6, s.X, error)
                    || !st.BindDouble(7, s.Y, error)
                    || !st.BindDouble(8, s.RotationDeg, error)
                    || !st.BindOptDouble(9, s.StepHeight, error)
                    || !st.BindOptDouble(10, s.TreadWidth, error)
                    || !st.BindOptDouble(11, s.FlightWidth, error)
                    || !st.BindOptDouble(12, s.LandingWidth, error)
                    || !st.BindOptDouble(13, s.TotalWidth, error)
                    || !st.BindOptInt(14, s.Steps1, error)
                    || !st.BindOptInt(15, s.Steps2, error)
                    || !st.BindOptInt(16, s.FloorType, error)
                    || !st.BindOptText(17, s.FloorTypeName, error)
                    || !mirrorOk
                    || !st.BindOptDouble(19, s.Scale, error)
                    || !st.BindOptText(20, s.ExtraJson, error)
                    || !st.BindDouble(21, s.MinX, error)
                    || !st.BindDouble(22, s.MinY, error)
                    || !st.BindDouble(23, s.MaxX, error)
                    || !st.BindDouble(24, s.MaxY, error)
                    || !st.StepDone(error))
                {
                    return false;
                }
                ++counts.Stairs;
            }
        }

        // 6. tch_dimensions
        {
            ScopedSqliteStmt st;
            const char* sql =
                "INSERT INTO tch_dimensions("
                "handle,dxf_name,layer,space,layout_name,x,y,z,angle_deg,offset_val,"
                "is_arc,arc_radius,measured_radius,span_count,total_span,"
                "spans_json,text_overrides_json,flags,scale,minx,miny,maxx,maxy"
                ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            if (!st.Prepare(db, sql, error)) return false;
            for (const auto& d : snap.TchDimensions)
            {
                if (!d.BlockName.empty()) continue;

                JsonWriter sw;
                sw.BeginArray();
                for (double sp : d.Spans) sw.Double(sp, 3);
                sw.EndArray();

                std::string overridesJson;
                if (!d.TextOverrides.empty())
                {
                    JsonWriter tw;
                    tw.BeginObject();
                    for (const auto& [idx, txt] : d.TextOverrides)
                        tw.KeyString(std::to_string(idx), txt);
                    tw.EndObject();
                    overridesJson = tw.Take();
                }

                if (!st.BindText(1, FormatHandleHex(d.Handle), error)
                    || !st.BindText(2, d.DxfName, error)
                    || !st.BindText(3, d.Layer, error)
                    || !st.BindText(4, d.Space, error)
                    || !st.BindText(5, d.LayoutName, error)
                    || !st.BindDouble(6, d.X, error)
                    || !st.BindDouble(7, d.Y, error)
                    || !st.BindDouble(8, d.Z, error)
                    || !st.BindDouble(9, d.AngleDeg, error)
                    || !st.BindDouble(10, d.OffsetVal, error)
                    || !st.BindInt(11, d.IsArc ? 1 : 0, error)
                    || !st.BindOptDouble(12, d.ArcRadius, error)
                    || !st.BindOptDouble(13, d.MeasuredRadius, error)
                    || !st.BindInt(14, static_cast<int>(d.Spans.size()), error)
                    || !st.BindDouble(15, d.TotalSpan, error)
                    || !st.BindText(16, sw.Str(), error)
                    || !st.BindOptText(17, overridesJson, error)
                    || !st.BindInt(18, d.Flags, error)
                    || !st.BindDouble(19, d.Scale, error)
                    || !st.BindDouble(20, d.MinX, error)
                    || !st.BindDouble(21, d.MinY, error)
                    || !st.BindDouble(22, d.MaxX, error)
                    || !st.BindDouble(23, d.MaxY, error)
                    || !st.StepDone(error))
                {
                    return false;
                }
                ++counts.Dimensions;
            }
        }

        // 7. tch_axes
        {
            ScopedSqliteStmt st;
            const char* sql =
                "INSERT INTO tch_axes("
                "group_handle,axis_index,layer,text_layer,space,layout_name,label,"
                "spacing,offset_from_first,x1,y1,x2,y2,orientation,coord,"
                "scale,show_text,flags,minx,miny,maxx,maxy"
                ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            if (!st.Prepare(db, sql, error)) return false;
            for (const auto& ax : snap.TchAxes)
            {
                if (!ax.BlockName.empty()) continue;
                if (!st.BindText(1, FormatHandleHex(ax.GroupHandle), error)
                    || !st.BindInt(2, ax.AxisIndex, error)
                    || !st.BindText(3, ax.Layer, error)
                    || !st.BindOptText(4, ax.TextLayer, error)
                    || !st.BindText(5, ax.Space, error)
                    || !st.BindText(6, ax.LayoutName, error)
                    || !st.BindOptText(7, ax.Label, error)
                    || !st.BindDouble(8, ax.Spacing, error)
                    || !st.BindDouble(9, ax.OffsetFromFirst, error)
                    || !st.BindDouble(10, ax.X1, error)
                    || !st.BindDouble(11, ax.Y1, error)
                    || !st.BindDouble(12, ax.X2, error)
                    || !st.BindDouble(13, ax.Y2, error)
                    || !st.BindText(14, ax.Orientation, error)
                    || !st.BindOptDouble(15, ax.Coord, error)
                    || !st.BindDouble(16, ax.Scale, error)
                    || !st.BindInt(17, ax.ShowText ? 1 : 0, error)
                    || !st.BindInt(18, ax.Flags, error)
                    || !st.BindDouble(19, ax.MinX, error)
                    || !st.BindDouble(20, ax.MinY, error)
                    || !st.BindDouble(21, ax.MaxX, error)
                    || !st.BindDouble(22, ax.MaxY, error)
                    || !st.StepDone(error))
                {
                    return false;
                }
                ++counts.Axes;
            }
        }

        // 8. tch_sheets
        {
            ScopedSqliteStmt st;
            const char* sql =
                "INSERT INTO tch_sheets("
                "handle,layer,space,layout_name,title,row_count,col_count,"
                "x,y,rotation_deg,cells_json,minx,miny,maxx,maxy"
                ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            if (!st.Prepare(db, sql, error)) return false;
            for (const auto& sh : snap.TchSheets)
            {
                if (!sh.BlockName.empty()) continue;

                JsonWriter cw;
                cw.BeginArray();
                for (const auto& row : sh.Cells)
                {
                    cw.BeginArray();
                    for (const auto& cell : row) cw.String(cell);
                    cw.EndArray();
                }
                cw.EndArray();

                if (!st.BindText(1, FormatHandleHex(sh.Handle), error)
                    || !st.BindText(2, sh.Layer, error)
                    || !st.BindText(3, sh.Space, error)
                    || !st.BindText(4, sh.LayoutName, error)
                    || !st.BindOptText(5, sh.Title, error)
                    || !st.BindInt(6, sh.RowCount, error)
                    || !st.BindInt(7, sh.ColCount, error)
                    || !st.BindDouble(8, sh.X, error)
                    || !st.BindDouble(9, sh.Y, error)
                    || !st.BindDouble(10, sh.RotationDeg, error)
                    || !st.BindText(11, cw.Str(), error)
                    || !st.BindDouble(12, sh.MinX, error)
                    || !st.BindDouble(13, sh.MinY, error)
                    || !st.BindDouble(14, sh.MaxX, error)
                    || !st.BindDouble(15, sh.MaxY, error)
                    || !st.StepDone(error))
                {
                    return false;
                }
                ++counts.Sheets;
            }
        }

        return true;
    }

    void CollectTchReadableTexts(const CadSemanticSnapshot& snap,
                                 std::vector<ExtractedText>& out,
                                 std::unordered_set<uint64_t>& emittedHandles)
    {
        auto pushText = [&](uint64_t handle, const std::string& kind, const std::string& content,
                            const std::string& layer, const std::string& space, const std::string& layout,
                            double x, double y, double h, double rotDeg) {
            const std::string trimmed = TrimText(content);
            if (trimmed.empty()) return;
            ExtractedText t;
            t.OwnerHandle = FormatHandleHex(handle);
            t.SourceKind  = kind;
            t.Content     = trimmed;
            t.Layer       = layer.empty() ? "0" : layer;
            t.Space       = space.empty() ? "model" : space;
            t.LayoutName  = layout.empty() ? "Model" : layout;
            t.X = x; t.Y = y;
            t.Height      = h > 0.0 ? h : 0.0;
            t.RotationDeg = rotDeg;
            MakeApproxTextBox(x, y, t.Height, trimmed, t.MinX, t.MinY, t.MaxX, t.MaxY);
            out.push_back(std::move(t));
            if (handle != 0) emittedHandles.insert(handle);
        };

        // 1. TCH_TEXT / TCH_MTEXT / TCH_ELEVATION / TCH_MULTILEADER
        for (const auto& tt : snap.TchTexts)
        {
            if (!tt.BlockName.empty()) continue;
            const std::string kind = (tt.SourceKind == "TCH_MULTILEADER") ? "TCH_SYMBOL" : tt.SourceKind;
            pushText(tt.OwnerHandle, kind, tt.Content, tt.Layer, tt.Space, tt.LayoutName,
                     tt.X, tt.Y, tt.Height, tt.RotationDeg);
        }

        // 2. TCH_OPENING 门窗编号
        for (const auto& o : snap.TchOpenings)
        {
            if (!o.BlockName.empty()) continue;
            emittedHandles.insert(o.Handle);
            if (!o.Label.empty())
            {
                const double lx = (std::abs(o.LabelX) > 1e-6 || std::abs(o.LabelY) > 1e-6) ? o.LabelX : o.X;
                const double ly = (std::abs(o.LabelX) > 1e-6 || std::abs(o.LabelY) > 1e-6) ? o.LabelY : o.Y;
                const std::string lyr = !o.LabelLayer.empty() ? o.LabelLayer : o.Layer;
                pushText(o.Handle, "TCH_OPENING", o.Label, lyr, o.Space, o.LayoutName,
                         lx, ly, o.LabelHeight * o.Scale, o.AngleRad * kRadToDeg);
            }
        }

        // 3. TCH_AXIS_LABEL 轴号
        for (const auto& ax : snap.TchAxes)
        {
            if (!ax.BlockName.empty()) continue;
            emittedHandles.insert(ax.GroupHandle);
            if (ax.ShowText && !ax.Label.empty())
            {
                const std::string lyr = !ax.TextLayer.empty() ? ax.TextLayer : ax.Layer;
                pushText(ax.GroupHandle, "TCH_AXIS_LABEL", ax.Label, lyr, ax.Space, ax.LayoutName,
                         ax.X1, ax.Y1, 3.5 * ax.Scale, 0.0);
            }
        }

        // 4. TCH_SPACE 房间名
        for (const auto& sp : snap.TchSpaces)
        {
            if (!sp.BlockName.empty()) continue;
            emittedHandles.insert(sp.Handle);
            if (!sp.Name.empty())
            {
                pushText(sp.Handle, "TCH_SPACE", sp.Name, sp.Layer, sp.Space, sp.LayoutName,
                         sp.CentroidX, sp.CentroidY, 3.5 * sp.Scale, 0.0);
            }
        }

        // 5. TCH_DIMENSION2 尺寸文字
        for (const auto& d : snap.TchDimensions)
        {
            if (!d.BlockName.empty()) continue;
            emittedHandles.insert(d.Handle);
            const double angRad = d.AngleDeg / kRadToDeg;
            const double ux = std::cos(angRad), uy = std::sin(angRad);
            const double nx = -uy, ny = ux;
            const double off = d.OffsetVal * d.Scale;
            double acc = 0.0;
            for (size_t i = 0; i < d.Spans.size(); ++i)
            {
                const double span = d.Spans[i];
                const double mid = acc + span * 0.5;
                acc += span;
                std::string label;
                if (auto it = d.TextOverrides.find(static_cast<int>(i)); it != d.TextOverrides.end() && !it->second.empty())
                {
                    label = it->second;
                    const std::string num = FormatSpanText(span);
                    size_t pos = 0;
                    while ((pos = label.find("<>", pos)) != std::string::npos)
                    {
                        label.replace(pos, 2, num);
                        pos += num.size();
                    }
                }
                else
                    label = FormatSpanText(span);
                const double px = d.X + ux * mid + nx * off;
                const double py = d.Y + uy * mid + ny * off;
                pushText(d.Handle, "TCH_DIMENSION2", label, d.Layer, d.Space, d.LayoutName,
                         px, py, 3.0 * d.Scale, d.AngleDeg);
            }
        }

        // 6. TCH_SHEET 表格标题与非空单元格
        for (const auto& sh : snap.TchSheets)
        {
            if (!sh.BlockName.empty()) continue;
            emittedHandles.insert(sh.Handle);
            if (!sh.Title.empty())
                pushText(sh.Handle, "TCH_SHEET", sh.Title, sh.Layer, sh.Space, sh.LayoutName,
                         0.5 * (sh.MinX + sh.MaxX), sh.MaxY, 350.0, sh.RotationDeg);
            const int nr = std::max(1, sh.RowCount);
            const int nc = std::max(1, sh.ColCount);
            const double w = sh.MaxX - sh.MinX;
            const double h = sh.MaxY - sh.MinY;
            for (size_t r = 0; r < sh.Cells.size(); ++r)
            {
                for (size_t c = 0; c < sh.Cells[r].size(); ++c)
                {
                    if (sh.Cells[r][c].empty()) continue;
                    const double cx = sh.MinX + (static_cast<double>(c) + 0.5) / nc * w;
                    const double cy = sh.MaxY - (static_cast<double>(r) + 0.5) / nr * h;
                    pushText(sh.Handle, "TCH_SHEET", sh.Cells[r][c], sh.Layer, sh.Space, sh.LayoutName,
                             cx, cy, 250.0, sh.RotationDeg);
                }
            }
        }
    }
}
