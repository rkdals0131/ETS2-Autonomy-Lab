#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

struct LanePath {
    struct Point {double x,y,s;};
    // d: distance to the polyline; offset: the same distance signed, left of the path positive.
    struct Projection {double s,d;size_t segment;double offset;};
    std::vector<Point> points;
    std::vector<double> headings; // per segment, unwrapped
    double width=0,progress=0;
    bool tracked=false;
    // Rolling provider files (map_lane_provider --follow): arc length of points.front()
    // in the provider's path (start_s), its path_id and the revision loaded last.
    double start_s=0;
    long long path_id=0,revision=0;
    // Ego search span past progress. The ego moves <1 m per 20 ms cycle; a
    // nearest point at the span end or off the lane falls back to the full scan.
    static constexpr double track_window_m=100;
    // A newer revision continues this path from the point at the same provider arc length.
    static constexpr double splice_tolerance_m=.01;
    explicit LanePath(const std::string& file) {
        const auto data=read(file);
        width=data.at("lane_width_m").get<double>();
        if(!std::isfinite(width) || width<=0) throw std::runtime_error("Invalid lane width");
        start_s=data.value("start_s",0.0);path_id=data.value("path_id",0LL);revision=data.value("revision",0LL);
        load(coordinates(data));
    }
    LanePath(const std::vector<std::array<double,2>>& xy,double lane_width):width(lane_width) {
        if(!std::isfinite(width) || width<=0) throw std::runtime_error("Invalid lane width");
        load(xy);
    }
    Projection project(double x,double y,double from=0,double to=std::numeric_limits<double>::infinity()) const {
        Projection best{0,std::numeric_limits<double>::infinity(),0,0};
        // First segment whose end is at or past from, as the full scan's b.s<from skip.
        const auto first=std::lower_bound(points.begin(),points.end(),from,[](const Point& p,double s){return p.s<s;});
        for(size_t i=first==points.begin()?0:size_t(first-points.begin())-1;i+1<points.size() && points[i].s<=to;++i) {
            const auto& a=points[i];const auto& b=points[i+1];
            const auto dx=b.x-a.x,dy=b.y-a.y,length=b.s-a.s;
            const auto t=std::clamp(((x-a.x)*dx+(y-a.y)*dy)/(length*length),0.0,1.0);
            const auto distance=std::hypot(x-a.x-t*dx,y-a.y-t*dy);
            if(distance<best.d) {
                // Distance to the path itself, not to the segment's infinite line,
                // so points beyond either end are not reported as on-lane.
                best={a.s+t*length,distance,i,std::copysign(distance,dx*(y-a.y)-dy*(x-a.x))};
            }
        }
        return best;
    }
    // Ego projection: monotonic progress, searched in a window ahead of it.
    Projection track(double x,double y) {
        auto p=project(x,y,progress,tracked?progress+track_window_m:std::numeric_limits<double>::infinity());
        if(tracked && (p.d>width || p.s>=progress+track_window_m)) p=project(x,y,progress);
        tracked=true;progress=std::max(progress,p.s);
        return p;
    }
    Point at(double s) const {
        // Continue the last segment past the end. A lookahead clamped to the end
        // point would shrink toward the vehicle and saturate steering there.
        s=std::max(s,0.0);
        const auto upper=std::upper_bound(points.begin(),points.end(),s,[](double s,const Point& p){return s<p.s;});
        const auto last=upper==points.end()?points.end()-1:upper;
        const auto& a=*(last-1);const auto& b=*last;const auto t=(s-a.s)/(b.s-a.s);
        return {a.x+t*(b.x-a.x),a.y+t*(b.y-a.y),s};
    }
    // The lane at s from a local least-squares quadratic through the points within
    // fit_half_m (tricube weights in arc length, so points enter and leave the fit
    // smoothly). The map lanes are float32 points 1.5 m apart (2-4 mm steps 30 km from
    // the origin): single segments turn by +-3 mrad, and steering on them shook the
    // truck at 3 Hz (2026-10-21). A quadratic follows arcs and clothoids without bias.
    // Past either end the lane continues straight along the end tangent.
    double fit_half_m=8;
    struct Local {double x,y,heading,curvature;};
    Local local(double s) const {
        const auto end=points.back().s,c=std::clamp(s,0.0,end);
        const auto& origin=at(c);
        double m[5]{},px[3]{},py[3]{};
        const auto first=std::lower_bound(points.begin(),points.end(),c-fit_half_m,[](const Point& p,double v){return p.s<v;});
        for(auto i=first;i!=points.end() && i->s<=c+fit_half_m;++i) {
            const auto t=i->s-c,u=std::abs(t)/fit_half_m,w=std::pow(1-u*u*u,3);
            double tk=w;
            for(int k=0;k<5;++k) {m[k]+=tk;if(k<3) {px[k]+=tk*(i->x-origin.x);py[k]+=tk*(i->y-origin.y);}tk*=t;}
        }
        // Normal equations [m0 m1 m2; m1 m2 m3; m2 m3 m4] q = p, by Cramer's rule.
        const auto det3=[](double a,double b,double c,double d,double e,double f,double g,double h,double k) {return a*(e*k-f*h)-b*(d*k-f*g)+c*(d*h-e*g);};
        const auto det=det3(m[0],m[1],m[2],m[1],m[2],m[3],m[2],m[3],m[4]);
        Local out{origin.x,origin.y,segment_heading(c),0};
        if(std::abs(det)>1e-9*std::pow(m[2],3)) {
            const auto solve=[&](const double* r) {
                return std::array{det3(r[0],m[1],m[2],r[1],m[2],m[3],r[2],m[3],m[4])/det,det3(m[0],r[0],m[2],m[1],r[1],m[3],m[2],r[2],m[4])/det,
                                  det3(m[0],m[1],r[0],m[1],m[2],r[1],m[2],m[3],r[2])/det};
            };
            const auto qx=solve(px),qy=solve(py);
            const auto speed=std::hypot(qx[1],qy[1]);
            if(speed>0) {
                out={origin.x+qx[0],origin.y+qy[0],out.heading+std::remainder(std::atan2(qy[1],qx[1])-out.heading,2*std::numbers::pi),
                     2*(qx[1]*qy[2]-qy[1]*qx[2])/(speed*speed*speed)};
            }
        }
        if(s!=c) {out.x+=(s-c)*std::cos(out.heading);out.y+=(s-c)*std::sin(out.heading);out.curvature=0;}
        return out;
    }
    // Tangent heading (unwrapped), smoothed as local().
    double heading(double s) const {return local(s).heading;}
    // Signed lateral offset (left positive) of (x, y) from the smoothed lane at s, its projection.
    double offset(double x,double y,double s) const {
        const auto l=local(s);
        return -(x-l.x)*std::sin(l.heading)+(y-l.y)*std::cos(l.heading);
    }
    // Segment headings at segment midpoints, interpolated linearly (no smoothing).
    double segment_heading(double s) const {
        const auto mid=[&](size_t i){return (points[i].s+points[i+1].s)/2;};
        const auto n=headings.size();
        if(s<=mid(0)) return headings.front();
        if(s>=mid(n-1)) return headings.back();
        const auto upper=std::upper_bound(points.begin()+1,points.end()-1,s,[](double s,const Point& p){return s<p.s;});
        auto i=size_t(upper-points.begin())-1; // segment containing s
        if(s<mid(i)) --i;
        const auto t=(s-mid(i))/(mid(i+1)-mid(i));
        return headings[i]+t*(headings[i+1]-headings[i]);
    }
    // Mean curvature over [s-w/2, s+w/2] of the smoothed heading; zero once past the end.
    double curvature(double s,double w) const {return (heading(s+w/2)-heading(s-w/2))/w;}
    double remaining() const {return points.back().s-progress;}
    // Pure pursuit curvature toward the lane point lookahead past progress.
    double pursuit(double x,double y,double yaw,double lookahead) const {
        const auto target=at(progress+lookahead);const auto dx=target.x-x,dy=target.y-y;
        const auto distance2=dx*dx+dy*dy;
        const auto lateral=-std::sin(yaw)*dx+std::cos(yaw)*dy;
        return distance2>0?2*lateral/distance2:0;
    }
    struct Target {double curvature,remaining,error;};
    Target follow(double x,double y,double yaw,double lookahead) {
        const auto p=track(x,y);
        return {pursuit(x,y,yaw,lookahead),remaining(),p.d};
    }
    // Appends points after the end. Existing points, their s and headings, progress and
    // the tracking window are unchanged; lookups up to fit_half_m before the old end
    // give the same values, only the extrapolation beyond it is replaced. A vehicle whose
    // lookahead and curvature window stay short of the old end sees no change. Returns the count added.
    size_t extend(const std::vector<std::array<double,2>>& xy) {
        const auto before=points.size();
        append(xy);
        return points.size()-before;
    }
    // Splices a newer revision of the provider file: its points from the one at this
    // path's last provider arc length on (start_s + s) are appended. Returns the count
    // added, 0 for an older or the same revision. Throws when the file does not continue
    // this path (another path_id, a reset, or more than the provider trimmed ahead of the
    // vehicle); the caller then loads the file as a new LanePath.
    size_t extend(const std::string& file) {
        const auto data=read(file);
        const auto next=data.value("revision",0LL);
        if(next<=revision) return 0;
        if(data.value("path_id",0LL)!=path_id) throw std::runtime_error("Lane file belongs to another path");
        const auto end=start_s+points.back().s;
        const auto all=coordinates(data);
        double s=data.value("start_s",0.0);
        for(size_t i=0;i<all.size();++i) {
            if(i>0) s+=std::hypot(all[i][0]-all[i-1][0],all[i][1]-all[i-1][1]);
            if(s>end+splice_tolerance_m) break;
            if(std::abs(s-end)<=splice_tolerance_m && std::hypot(all[i][0]-points.back().x,all[i][1]-points.back().y)<=splice_tolerance_m) {
                const auto added=extend(std::vector<std::array<double,2>>(all.begin()+std::ptrdiff_t(i)+1,all.end()));
                revision=next;
                return added;
            }
        }
        throw std::runtime_error("Lane file revision does not continue this path");
    }
private:
    static nlohmann::json read(const std::string& file) {
        std::ifstream input(file);
        if(!input) throw std::runtime_error("Cannot open lane file "+file);
        nlohmann::json data;input>>data;
        if(data.at("frame_id")!="world") throw std::runtime_error("Lane path must use ROS world coordinates");
        return data;
    }
    static std::vector<std::array<double,2>> coordinates(const nlohmann::json& data) {
        std::vector<std::array<double,2>> out;
        for(const auto& p:data.at("points")) out.push_back({p.at(0).get<double>(),p.at(1).get<double>()});
        return out;
    }
    void load(const std::vector<std::array<double,2>>& xy) {
        append(xy);
        if(points.size()<2) throw std::runtime_error("Lane path needs two distinct points");
    }
    void append(const std::vector<std::array<double,2>>& xy) {
        for(const auto& [x,y]:xy) if(!std::isfinite(x) || !std::isfinite(y)) throw std::runtime_error("Nonfinite lane point");
        for(const auto& [x,y]:xy) {
            const auto distance=points.empty()?0:std::hypot(x-points.back().x,y-points.back().y);
            if(!points.empty() && distance==0) continue;
            points.push_back({x,y,points.empty()?0:points.back().s+distance});
            if(points.size()<2) continue;
            const auto& a=points[points.size()-2];
            const auto h=std::atan2(y-a.y,x-a.x);
            headings.push_back(headings.empty()?h:headings.back()+std::remainder(h-headings.back(),2*std::numbers::pi));
        }
    }
};
