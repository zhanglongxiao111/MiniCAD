#include "TchSemanticExport.h"
#include "JsonWriter.h"
#include "sqlite3.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace MiniCAD::SaTool
{
    namespace
    {
        constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;

        void BindTextOrNull(sqlite3_stmt* st, int idx, const std::string& s)
        {
            if (s.empty())
                sqlite3_bind_null(st, idx);
            else
                sqlite3_bind_text(st, idx, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
        }

        void BindTextNonNull(sqlite3_stmt* st, int idx, const std::string& s)
        {
            sqlite3_bind_text(st, idx, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
        }

        void BindOptDouble(sqlite3_stmt* st, int idx, const std::optional<double>& v)
        {
            if (v.has_value() && std::isfinite(*v))
                sqlite3_bind_double(st, idx, *v);
            else
                sqlite3_bind_null(st, idx);
        }

        void BindOptInt(sqlite3_stmt* st, int idx, const std::optional<int>& v)
        {
            if (v.has_value())
                sqlite3_bind_int(st, idx, *v);
            else
                sqlite3_bind_null(st, idx);
        }

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

        bool PrepareStmt(sqlite3* db, const char* sql, sqlite3_stmt*& stmt, std::string* error)
        {
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
            {
                if (error) *error = std::string("SQLite prepare 失败：") + sqlite3_errmsg(db);
                return false;
            }
            return true;
        }

        bool StepReset(sqlite3* db, sqlite3_stmt* stmt, std::string* error)
        {
            const int rc = sqlite3_step(stmt);
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            if (rc != SQLITE_DONE)
            {
                if (error) *error = std::string("SQLite step 失败：") + sqlite3_errmsg(db);
                return false;
            }
            return true;
        }
    }

    bool WriteTchSemanticTables(sqlite3* db, const CadSemanticSnapshot& snap,
                                TchExportCounts& counts, std::string* error)
    {
        counts = {};

        // 1. tch_walls
        {
            sqlite3_stmt* st = nullptr;
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
            if (!PrepareStmt(db, sql, st, error)) return false;
            for (const auto& w : snap.TchWalls)
            {
                if (!w.BlockName.empty()) continue;
                BindTextNonNull(st, 1, FormatHandleHex(w.Handle));
                BindTextNonNull(st, 2, w.DxfName);
                BindTextNonNull(st, 3, w.Layer);
                BindTextNonNull(st, 4, w.Space);
                BindTextNonNull(st, 5, w.LayoutName);
                sqlite3_bind_double(st, 6, w.StartX);
                sqlite3_bind_double(st, 7, w.StartY);
                sqlite3_bind_double(st, 8, w.StartZ);
                sqlite3_bind_double(st, 9, w.EndX);
                sqlite3_bind_double(st, 10, w.EndY);
                sqlite3_bind_double(st, 11, w.EndZ);
                sqlite3_bind_double(st, 12, w.Length);
                sqlite3_bind_double(st, 13, w.ArcAngleRad);
                if (w.HasArc)
                {
                    sqlite3_bind_double(st, 14, w.ArcCx);
                    sqlite3_bind_double(st, 15, w.ArcCy);
                    sqlite3_bind_double(st, 16, w.ArcRadius);
                }
                else
                {
                    sqlite3_bind_null(st, 14);
                    sqlite3_bind_null(st, 15);
                    sqlite3_bind_null(st, 16);
                }
                sqlite3_bind_double(st, 17, w.LeftWidth);
                sqlite3_bind_double(st, 18, w.RightWidth);
                sqlite3_bind_double(st, 19, w.TotalThickness);
                if (w.HasHeight) sqlite3_bind_double(st, 20, w.Height);
                else             sqlite3_bind_null(st, 20);
                sqlite3_bind_int(st, 21, w.MaterialCode);
                BindTextNonNull(st, 22, w.MaterialName);
                sqlite3_bind_int64(st, 23, static_cast<sqlite3_int64>(w.Flags));
                sqlite3_bind_double(st, 24, w.Scale);
                sqlite3_bind_int(st, 25, w.OpeningCount);

                JsonWriter jw;
                jw.BeginArray();
                for (double j : w.Joins) jw.Double(j, 3);
                jw.EndArray();
                BindTextNonNull(st, 26, jw.Str());

                sqlite3_bind_int(st, 27, w.HasConnections ? 1 : 0);

                JsonWriter cw;
                cw.BeginArray();
                for (uint64_t ch : w.ConnectedHandles)
                    if (ch != 0) cw.String(FormatHandleHex(ch));
                cw.EndArray();
                BindTextNonNull(st, 28, cw.Str());

                sqlite3_bind_double(st, 29, w.MinX);
                sqlite3_bind_double(st, 30, w.MinY);
                sqlite3_bind_double(st, 31, w.MaxX);
                sqlite3_bind_double(st, 32, w.MaxY);

                if (!StepReset(db, st, error)) { sqlite3_finalize(st); return false; }
                ++counts.Walls;
            }
            sqlite3_finalize(st);
        }

        // 2. tch_openings
        {
            sqlite3_stmt* st = nullptr;
            const char* sql =
                "INSERT INTO tch_openings("
                "handle,parent_handle,layer,label_layer,space,layout_name,"
                "kind_code,kind_name,label,x,y,sill_z,width,height,thickness,"
                "angle_rad,angle_deg,mirror,mirror_x,mirror_y,wall_handle,"
                "label_x,label_y,label_height,scale,flags,display_flags,"
                "minx,miny,maxx,maxy"
                ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            if (!PrepareStmt(db, sql, st, error)) return false;
            for (const auto& o : snap.TchOpenings)
            {
                if (!o.BlockName.empty()) continue;
                BindTextNonNull(st, 1, FormatHandleHex(o.Handle));
                BindTextOrNull(st, 2, FormatHandleHex(o.ParentHandle));
                BindTextNonNull(st, 3, o.Layer);
                BindTextOrNull(st, 4, o.LabelLayer);
                BindTextNonNull(st, 5, o.Space);
                BindTextNonNull(st, 6, o.LayoutName);
                sqlite3_bind_int(st, 7, o.KindCode);
                BindTextNonNull(st, 8, o.KindName);
                BindTextOrNull(st, 9, o.Label);
                sqlite3_bind_double(st, 10, o.X);
                sqlite3_bind_double(st, 11, o.Y);
                sqlite3_bind_double(st, 12, o.SillZ);
                sqlite3_bind_double(st, 13, o.Width);
                sqlite3_bind_double(st, 14, o.Height);
                sqlite3_bind_double(st, 15, o.Thickness);
                sqlite3_bind_double(st, 16, o.AngleRad);
                sqlite3_bind_double(st, 17, o.AngleRad * kRadToDeg);
                sqlite3_bind_int(st, 18, o.Mirror);
                sqlite3_bind_int(st, 19, o.MirrorX ? 1 : 0);
                sqlite3_bind_int(st, 20, o.MirrorY ? 1 : 0);
                BindTextOrNull(st, 21, FormatHandleHex(o.WallHandle));
                sqlite3_bind_double(st, 22, o.LabelX);
                sqlite3_bind_double(st, 23, o.LabelY);
                sqlite3_bind_double(st, 24, o.LabelHeight);
                sqlite3_bind_double(st, 25, o.Scale);
                sqlite3_bind_int(st, 26, o.Flags);
                sqlite3_bind_int(st, 27, o.DisplayFlags);
                sqlite3_bind_double(st, 28, o.MinX);
                sqlite3_bind_double(st, 29, o.MinY);
                sqlite3_bind_double(st, 30, o.MaxX);
                sqlite3_bind_double(st, 31, o.MaxY);

                if (!StepReset(db, st, error)) { sqlite3_finalize(st); return false; }
                ++counts.Openings;
            }
            sqlite3_finalize(st);
        }

        // 3. tch_columns
        {
            sqlite3_stmt* st = nullptr;
            const char* sql =
                "INSERT INTO tch_columns("
                "handle,layer,space,layout_name,x,y,base_z,height,"
                "material_code,material_name,scale,area_m2,outline_json,"
                "minx,miny,maxx,maxy"
                ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            if (!PrepareStmt(db, sql, st, error)) return false;
            for (const auto& c : snap.TchColumns)
            {
                if (!c.BlockName.empty()) continue;
                BindTextNonNull(st, 1, FormatHandleHex(c.Handle));
                BindTextNonNull(st, 2, c.Layer);
                BindTextNonNull(st, 3, c.Space);
                BindTextNonNull(st, 4, c.LayoutName);
                sqlite3_bind_double(st, 5, c.X);
                sqlite3_bind_double(st, 6, c.Y);
                sqlite3_bind_double(st, 7, c.BaseZ);
                sqlite3_bind_double(st, 8, c.Height);
                sqlite3_bind_int(st, 9, c.MaterialCode);
                BindTextNonNull(st, 10, c.MaterialName);
                sqlite3_bind_double(st, 11, c.Scale);
                if (c.HasArea) sqlite3_bind_double(st, 12, c.AreaM2);
                else           sqlite3_bind_null(st, 12);

                JsonWriter ow;
                ow.BeginArray();
                for (const auto& v : c.Outline)
                {
                    ow.BeginArray();
                    ow.Double(v.X, 3); ow.Double(v.Y, 3); ow.Double(v.Bulge, 6);
                    ow.EndArray();
                }
                ow.EndArray();
                BindTextNonNull(st, 13, ow.Str());

                sqlite3_bind_double(st, 14, c.MinX);
                sqlite3_bind_double(st, 15, c.MinY);
                sqlite3_bind_double(st, 16, c.MaxX);
                sqlite3_bind_double(st, 17, c.MaxY);

                if (!StepReset(db, st, error)) { sqlite3_finalize(st); return false; }
                ++counts.Columns;
            }
            sqlite3_finalize(st);
        }

        // 4. tch_spaces
        {
            sqlite3_stmt* st = nullptr;
            const char* sql =
                "INSERT INTO tch_spaces("
                "handle,layer,space,layout_name,name,area_m2,perimeter_mm,"
                "centroid_x,centroid_y,scale,outline_json,minx,miny,maxx,maxy"
                ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            if (!PrepareStmt(db, sql, st, error)) return false;
            for (const auto& s : snap.TchSpaces)
            {
                if (!s.BlockName.empty()) continue;
                BindTextNonNull(st, 1, FormatHandleHex(s.Handle));
                BindTextNonNull(st, 2, s.Layer);
                BindTextNonNull(st, 3, s.Space);
                BindTextNonNull(st, 4, s.LayoutName);
                BindTextOrNull(st, 5, s.Name);
                sqlite3_bind_double(st, 6, s.AreaM2);
                sqlite3_bind_double(st, 7, s.PerimeterMm);
                sqlite3_bind_double(st, 8, s.CentroidX);
                sqlite3_bind_double(st, 9, s.CentroidY);
                sqlite3_bind_double(st, 10, s.Scale);

                JsonWriter ow;
                ow.BeginArray();
                for (const auto& [px, py] : s.Outline)
                {
                    ow.BeginArray(); ow.Double(px, 3); ow.Double(py, 3); ow.EndArray();
                }
                ow.EndArray();
                BindTextNonNull(st, 11, ow.Str());

                sqlite3_bind_double(st, 12, s.MinX);
                sqlite3_bind_double(st, 13, s.MinY);
                sqlite3_bind_double(st, 14, s.MaxX);
                sqlite3_bind_double(st, 15, s.MaxY);

                if (!StepReset(db, st, error)) { sqlite3_finalize(st); return false; }
                ++counts.Spaces;
            }
            sqlite3_finalize(st);
        }

        // 5. tch_stairs
        {
            sqlite3_stmt* st = nullptr;
            const char* sql =
                "INSERT INTO tch_stairs("
                "handle,stair_kind,layer,space,layout_name,x,y,rotation_deg,"
                "step_height,tread_width,flight_width,landing_width,total_width,"
                "steps1,steps2,floor_type,floor_type_name,mirror,scale,extra_json,"
                "minx,miny,maxx,maxy"
                ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            if (!PrepareStmt(db, sql, st, error)) return false;
            for (const auto& s : snap.TchStairs)
            {
                if (!s.BlockName.empty()) continue;
                BindTextNonNull(st, 1, FormatHandleHex(s.Handle));
                BindTextNonNull(st, 2, s.StairKind);
                BindTextNonNull(st, 3, s.Layer);
                BindTextNonNull(st, 4, s.Space);
                BindTextNonNull(st, 5, s.LayoutName);
                sqlite3_bind_double(st, 6, s.X);
                sqlite3_bind_double(st, 7, s.Y);
                sqlite3_bind_double(st, 8, s.RotationDeg);
                BindOptDouble(st, 9, s.StepHeight);
                BindOptDouble(st, 10, s.TreadWidth);
                BindOptDouble(st, 11, s.FlightWidth);
                BindOptDouble(st, 12, s.LandingWidth);
                BindOptDouble(st, 13, s.TotalWidth);
                BindOptInt(st, 14, s.Steps1);
                BindOptInt(st, 15, s.Steps2);
                BindOptInt(st, 16, s.FloorType);
                BindTextOrNull(st, 17, s.FloorTypeName);
                if (s.Mirror.has_value()) sqlite3_bind_int(st, 18, *s.Mirror ? 1 : 0);
                else                      sqlite3_bind_null(st, 18);
                BindOptDouble(st, 19, s.Scale);
                BindTextOrNull(st, 20, s.ExtraJson);
                sqlite3_bind_double(st, 21, s.MinX);
                sqlite3_bind_double(st, 22, s.MinY);
                sqlite3_bind_double(st, 23, s.MaxX);
                sqlite3_bind_double(st, 24, s.MaxY);

                if (!StepReset(db, st, error)) { sqlite3_finalize(st); return false; }
                ++counts.Stairs;
            }
            sqlite3_finalize(st);
        }

        // 6. tch_dimensions
        {
            sqlite3_stmt* st = nullptr;
            const char* sql =
                "INSERT INTO tch_dimensions("
                "handle,dxf_name,layer,space,layout_name,x,y,z,angle_deg,offset_val,"
                "is_arc,arc_radius,measured_radius,span_count,total_span,"
                "spans_json,text_overrides_json,flags,scale,minx,miny,maxx,maxy"
                ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            if (!PrepareStmt(db, sql, st, error)) return false;
            for (const auto& d : snap.TchDimensions)
            {
                if (!d.BlockName.empty()) continue;
                BindTextNonNull(st, 1, FormatHandleHex(d.Handle));
                BindTextNonNull(st, 2, d.DxfName);
                BindTextNonNull(st, 3, d.Layer);
                BindTextNonNull(st, 4, d.Space);
                BindTextNonNull(st, 5, d.LayoutName);
                sqlite3_bind_double(st, 6, d.X);
                sqlite3_bind_double(st, 7, d.Y);
                sqlite3_bind_double(st, 8, d.Z);
                sqlite3_bind_double(st, 9, d.AngleDeg);
                sqlite3_bind_double(st, 10, d.OffsetVal);
                sqlite3_bind_int(st, 11, d.IsArc ? 1 : 0);
                BindOptDouble(st, 12, d.ArcRadius);
                BindOptDouble(st, 13, d.MeasuredRadius);
                sqlite3_bind_int(st, 14, static_cast<int>(d.Spans.size()));
                sqlite3_bind_double(st, 15, d.TotalSpan);

                JsonWriter sw;
                sw.BeginArray();
                for (double sp : d.Spans) sw.Double(sp, 3);
                sw.EndArray();
                BindTextNonNull(st, 16, sw.Str());

                if (d.TextOverrides.empty())
                {
                    sqlite3_bind_null(st, 17);
                }
                else
                {
                    JsonWriter tw;
                    tw.BeginObject();
                    for (const auto& [idx, txt] : d.TextOverrides)
                        tw.KeyString(std::to_string(idx), txt);
                    tw.EndObject();
                    BindTextNonNull(st, 17, tw.Str());
                }

                sqlite3_bind_int(st, 18, d.Flags);
                sqlite3_bind_double(st, 19, d.Scale);
                sqlite3_bind_double(st, 20, d.MinX);
                sqlite3_bind_double(st, 21, d.MinY);
                sqlite3_bind_double(st, 22, d.MaxX);
                sqlite3_bind_double(st, 23, d.MaxY);

                if (!StepReset(db, st, error)) { sqlite3_finalize(st); return false; }
                ++counts.Dimensions;
            }
            sqlite3_finalize(st);
        }

        // 7. tch_axes
        {
            sqlite3_stmt* st = nullptr;
            const char* sql =
                "INSERT INTO tch_axes("
                "group_handle,axis_index,layer,text_layer,space,layout_name,label,"
                "spacing,offset_from_first,x1,y1,x2,y2,orientation,coord,"
                "scale,show_text,flags,minx,miny,maxx,maxy"
                ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            if (!PrepareStmt(db, sql, st, error)) return false;
            for (const auto& ax : snap.TchAxes)
            {
                if (!ax.BlockName.empty()) continue;
                BindTextNonNull(st, 1, FormatHandleHex(ax.GroupHandle));
                sqlite3_bind_int(st, 2, ax.AxisIndex);
                BindTextNonNull(st, 3, ax.Layer);
                BindTextOrNull(st, 4, ax.TextLayer);
                BindTextNonNull(st, 5, ax.Space);
                BindTextNonNull(st, 6, ax.LayoutName);
                BindTextOrNull(st, 7, ax.Label);
                sqlite3_bind_double(st, 8, ax.Spacing);
                sqlite3_bind_double(st, 9, ax.OffsetFromFirst);
                sqlite3_bind_double(st, 10, ax.X1);
                sqlite3_bind_double(st, 11, ax.Y1);
                sqlite3_bind_double(st, 12, ax.X2);
                sqlite3_bind_double(st, 13, ax.Y2);
                BindTextNonNull(st, 14, ax.Orientation);
                BindOptDouble(st, 15, ax.Coord);
                sqlite3_bind_double(st, 16, ax.Scale);
                sqlite3_bind_int(st, 17, ax.ShowText ? 1 : 0);
                sqlite3_bind_int(st, 18, ax.Flags);
                sqlite3_bind_double(st, 19, ax.MinX);
                sqlite3_bind_double(st, 20, ax.MinY);
                sqlite3_bind_double(st, 21, ax.MaxX);
                sqlite3_bind_double(st, 22, ax.MaxY);

                if (!StepReset(db, st, error)) { sqlite3_finalize(st); return false; }
                ++counts.Axes;
            }
            sqlite3_finalize(st);
        }

        // 8. tch_sheets
        {
            sqlite3_stmt* st = nullptr;
            const char* sql =
                "INSERT INTO tch_sheets("
                "handle,layer,space,layout_name,title,row_count,col_count,"
                "x,y,rotation_deg,cells_json,minx,miny,maxx,maxy"
                ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            if (!PrepareStmt(db, sql, st, error)) return false;
            for (const auto& sh : snap.TchSheets)
            {
                if (!sh.BlockName.empty()) continue;
                BindTextNonNull(st, 1, FormatHandleHex(sh.Handle));
                BindTextNonNull(st, 2, sh.Layer);
                BindTextNonNull(st, 3, sh.Space);
                BindTextNonNull(st, 4, sh.LayoutName);
                BindTextOrNull(st, 5, sh.Title);
                sqlite3_bind_int(st, 6, sh.RowCount);
                sqlite3_bind_int(st, 7, sh.ColCount);
                sqlite3_bind_double(st, 8, sh.X);
                sqlite3_bind_double(st, 9, sh.Y);
                sqlite3_bind_double(st, 10, sh.RotationDeg);

                JsonWriter cw;
                cw.BeginArray();
                for (const auto& row : sh.Cells)
                {
                    cw.BeginArray();
                    for (const auto& cell : row) cw.String(cell);
                    cw.EndArray();
                }
                cw.EndArray();
                BindTextNonNull(st, 11, cw.Str());

                sqlite3_bind_double(st, 12, sh.MinX);
                sqlite3_bind_double(st, 13, sh.MinY);
                sqlite3_bind_double(st, 14, sh.MaxX);
                sqlite3_bind_double(st, 15, sh.MaxY);

                if (!StepReset(db, st, error)) { sqlite3_finalize(st); return false; }
                ++counts.Sheets;
            }
            sqlite3_finalize(st);
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
                    label = it->second;
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
