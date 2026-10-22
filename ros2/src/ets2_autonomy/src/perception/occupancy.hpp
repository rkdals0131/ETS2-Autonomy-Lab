#pragma once
// LiDAR occupancy around the truck fused with GT traffic (plan stage 5): what the LiDAR sees
// that ground truth does not explain (accidents, random encounters, anything on the road
// without an AI vehicle) and how far ahead along the lane the first of it is. No ROS here.
// - Ground: the lowest return in 2 m tiles, taken over the tiles within ground_tiles around
//   each (no plane assumed, so slopes and crests stay ground: a point is at most 4 m from the
//   window's far edge, 0.32 m at 8 %, 0.4 m at 10 %; two tiles would be 0.48 m at 8 %).
// - A return min_height..max_height above that ground is an obstacle point (below the
//   truck's height; bridges and signs above), within ground_band of it observed road.
// - Points on the truck and its trailer are dropped; an obstacle point inside a GT vehicle
//   (plus margin) is explained by it.
// - The grid (ego-aligned, x ahead) holds -1 unknown, 0 observed road, 60 explained, 100
//   unexplained obstacle; the path check counts a stretch as blocked when min_points
//   unexplained points lie within cluster_m along the path and half_width across.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace perception {

struct Point {double x,y,z;};
// A ground footprint: centre, heading, length along it and width.
struct Box {
    double x,y,yaw,length,width;
    bool contains(double px,double py,double margin=0) const {
        const double dx=px-x,dy=py-y,c=std::cos(yaw),s=std::sin(yaw);
        return std::abs(dx*c+dy*s)<=length/2+margin && std::abs(-dx*s+dy*c)<=width/2+margin;
    }
};

// A GT vehicle at another time: moved dt seconds along its heading at speed (dt < 0: back to
// when the clouds were taken, which trail GT by about 0.13 s, p95 0.5 s), lengthened by
// speed * spread to cover clouds taken over spread seconds.
inline Box moved(Box b,double speed,double dt,double spread=0) {
    b.x+=speed*dt*std::cos(b.yaw);b.y+=speed*dt*std::sin(b.yaw);b.length+=std::abs(speed)*spread;
    return b;
}

struct Params {
    double behind=20,ahead=100,side=20,cell=.5;   // grid extent (m) and cell size
    double tile=2;int ground_tiles=1;              // ground tiles and the window around each
    double min_height=.5,max_height=4.2,ground_band=.3;
    double own_margin=.4,object_margin=.6;
    double half_width=1.6,cluster_m=2;int min_points=3;
};

struct Result {
    int nx=0,ny=0;double origin_x=0,origin_y=0,yaw=0,cell=0;   // grid: cell (i,j) at origin + R(yaw)*((i+.5)c,(j+.5)c)
    std::vector<int8_t> cells;
    std::vector<Point> unexplained,explained;
    double blocked_m=std::numeric_limits<double>::infinity();      // first blocked stretch along the path
};

// ego: the truck's position and heading; path: world xy polyline ahead (the followed lane),
// its first point near the truck.
inline Result analyze(const std::vector<Point>& points,double ego_x,double ego_y,double ego_yaw,
                      const std::vector<Box>& objects,const std::vector<Box>& own,
                      const std::vector<std::array<double,2>>& path,const Params& p={}) {
    Result r;
    r.cell=p.cell;r.yaw=ego_yaw;
    r.nx=int(std::lround((p.behind+p.ahead)/p.cell));r.ny=int(std::lround(2*p.side/p.cell));
    const double c=std::cos(ego_yaw),s=std::sin(ego_yaw);
    r.origin_x=ego_x-p.behind*c+p.side*s;r.origin_y=ego_y-p.behind*s-p.side*c;
    r.cells.assign(size_t(r.nx)*r.ny,-1);
    // Ego frame: u ahead, v left.
    const auto local=[&](double x,double y) {const double dx=x-ego_x,dy=y-ego_y;return std::array<double,2>{dx*c+dy*s,-dx*s+dy*c};};
    const int tx=int(std::ceil((p.behind+p.ahead)/p.tile)),ty=int(std::ceil(2*p.side/p.tile));
    const double inf=std::numeric_limits<double>::infinity();
    std::vector<double> lowest(size_t(tx)*ty,inf);
    struct Kept {Point w;double u,v;int ti,tj;};
    std::vector<Kept> kept;kept.reserve(points.size());
    for(const auto& q:points) {
        if(!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) continue;
        const auto [u,v]=local(q.x,q.y);
        if(u< -p.behind || u>=p.ahead || v< -p.side || v>=p.side) continue;
        if(std::any_of(own.begin(),own.end(),[&](const Box& b){return b.contains(q.x,q.y,p.own_margin);})) continue;
        const int ti=int((u+p.behind)/p.tile),tj=int((v+p.side)/p.tile);
        auto& low=lowest[size_t(ti)*ty+tj];low=std::min(low,q.z);
        kept.push_back({q,u,v,ti,tj});
    }
    std::vector<double> ground(lowest.size(),inf);
    for(int i=0;i<tx;++i) for(int j=0;j<ty;++j) {
        double g=inf;
        for(int a=std::max(0,i-p.ground_tiles);a<=std::min(tx-1,i+p.ground_tiles);++a)
            for(int b=std::max(0,j-p.ground_tiles);b<=std::min(ty-1,j+p.ground_tiles);++b) g=std::min(g,lowest[size_t(a)*ty+b]);
        ground[size_t(i)*ty+j]=g;
    }
    const auto set=[&](double u,double v,int8_t value) {
        const int i=int((u+p.behind)/p.cell),j=int((v+p.side)/p.cell);
        if(i<0 || j<0 || i>=r.nx || j>=r.ny) return;
        auto& cell=r.cells[size_t(j)*r.nx+i];   // row-major by v (OccupancyGrid: x fastest)
        cell=std::max(cell,value);
    };
    for(const auto& k:kept) {
        const double h=k.w.z-ground[size_t(k.ti)*ty+k.tj];
        if(h<=p.ground_band) {set(k.u,k.v,0);continue;}
        if(h<p.min_height || h>p.max_height) continue;
        const bool explained=std::any_of(objects.begin(),objects.end(),[&](const Box& b){return b.contains(k.w.x,k.w.y,p.object_margin);});
        (explained?r.explained:r.unexplained).push_back(k.w);
        set(k.u,k.v,explained?60:100);
    }
    // Unexplained points within half_width of the path, by arc length.
    if(path.size()>=2) {
        std::vector<double> along;
        for(const auto& q:r.unexplained) {
            double best=inf,best_s=0,s0=0;
            for(size_t i=0;i+1<path.size();++i) {
                const double ax=path[i][0],ay=path[i][1],bx=path[i+1][0],by=path[i+1][1];
                const double dx=bx-ax,dy=by-ay,l2=dx*dx+dy*dy,l=std::sqrt(l2);
                const double t=l2>0?std::clamp(((q.x-ax)*dx+(q.y-ay)*dy)/l2,0.0,1.0):0;
                const double d=std::hypot(ax+t*dx-q.x,ay+t*dy-q.y);
                if(d<best) {best=d;best_s=s0+t*l;}
                s0+=l;
            }
            if(best<=p.half_width) along.push_back(best_s);
        }
        std::sort(along.begin(),along.end());
        for(size_t i=0;i+p.min_points<=along.size();++i)
            if(along[i+p.min_points-1]-along[i]<=p.cluster_m) {r.blocked_m=along[i];break;}
    }
    return r;
}

} // namespace perception
