#pragma once
// Game-free parts of the HUD decal (docs/24 §5): the path-aligned BEV mask, its
// world->uv affine and the main camera projection used for the draw bounds.
// World coordinates are SCS: X east, Y up, Z south; the mask lies in X-Z.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace ot::overlay {
inline constexpr int mask_width=640,mask_height=128;
inline constexpr double mask_cell=.25,mask_back=4,edge_m=.15;
inline constexpr double mask_length=mask_width*mask_cell,mask_lateral=mask_height*mask_cell; // 160 m x 32 m

// R = coverage (edge smoothstep, arc-length fade baked in), G = arc length / 160 m.
struct Mask {
    std::array<double,2> origin{},axis{1,0},normal{0,1}; // path start, along and across the mask
    double centre=0;                                     // lateral offset of the mask centre line
    std::vector<uint8_t> texels;                         // R8G8, row-major, mask_height rows
    uint32_t points_used=0;
    bool clipped=false;                                  // part of the path left the 32 m band
    double length=0;                                     // arc length of the used points
    // uv = (affine[0]*x+affine[1]*z+affine[2], affine[3]*x+affine[4]*z+affine[5]) for x,z relative to origin.
    std::array<double,6> affine() const {
        return {axis[0]/mask_length,axis[1]/mask_length,mask_back/mask_length,
                normal[0]/mask_lateral,normal[1]/mask_lateral,.5-centre/mask_lateral};
    }
    std::array<double,2> uv(double x,double z) const {
        const auto a=affine();const double dx=x-origin[0],dz=z-origin[1];
        return {a[0]*dx+a[1]*dz+a[2],a[3]*dx+a[4]*dz+a[5]};
    }
    // Centre of texel (column, row) in world X, Z.
    std::array<double,2> texel_world(int column,int row) const {
        const double along=-mask_back+(column+.5)*mask_cell,across=centre-mask_lateral/2+(row+.5)*mask_cell;
        return {origin[0]+along*axis[0]+across*normal[0],origin[1]+along*axis[1]+across*normal[1]};
    }
};
inline double smoothstep(double a,double b,double x) {
    const double t=std::clamp((x-a)/(b-a),0.0,1.0);return t*t*(3-2*t);
}
// points: SCS X,Z of the path from its start. Empty texels when fewer than two
// distinct points remain within the mask length.
inline Mask rasterize(const double (*points)[2],uint32_t count,double width_m,double fade_start=80,double fade_end=120) {
    Mask mask;mask.texels.assign(size_t(mask_width)*mask_height*2,0);
    if(count<2 || !(width_m>0)) return mask;
    mask.origin={points[0][0],points[0][1]};
    // Arc length per point; stop after the mask length (plus the far edge).
    std::vector<double> s(count,0.0);uint32_t used=1;
    for(uint32_t i=1;i<count;++i) {
        s[i]=s[i-1]+std::hypot(points[i][0]-points[i-1][0],points[i][1]-points[i-1][1]);
        used=i+1;
        if(s[i]>mask_length-mask_back+width_m) break;
    }
    // Axis along the chord to the farthest used point: a curve's sagitta about
    // its chord is a quarter of its offset from the start tangent.
    double ax=0,az=0;
    for(uint32_t i=used;i-->1;) {
        ax=points[i][0]-mask.origin[0];az=points[i][1]-mask.origin[1];
        if(std::hypot(ax,az)>.01) break;
    }
    const double norm=std::hypot(ax,az);
    if(!(norm>.01)) return mask;
    mask.axis={ax/norm,az/norm};mask.normal={-mask.axis[1],mask.axis[0]};
    mask.points_used=used;mask.length=s[used-1];
    // Centre the band on the lateral extent of the path inside the mask.
    double low=0,high=0;
    for(uint32_t i=0;i<used;++i) {
        const double dx=points[i][0]-mask.origin[0],dz=points[i][1]-mask.origin[1];
        const double along=dx*mask.axis[0]+dz*mask.axis[1];
        if(along<-mask_back || along>mask_length-mask_back) continue;
        const double across=dx*mask.normal[0]+dz*mask.normal[1];
        low=std::min(low,across);high=std::max(high,across);
    }
    const double half=width_m/2,room=mask_lateral/2-half;
    mask.centre=std::clamp((low+high)/2,-1e9,1e9);
    mask.clipped=high-mask.centre>room || mask.centre-low>room;
    // Distance and arc length of the nearest segment, per texel within reach.
    std::vector<float> distance(size_t(mask_width)*mask_height,1e9f),arc(distance.size(),0.f);
    const auto local=[&](uint32_t i) {
        const double dx=points[i][0]-mask.origin[0],dz=points[i][1]-mask.origin[1];
        return std::array<double,2>{dx*mask.axis[0]+dz*mask.axis[1]+mask_back,
                                    dx*mask.normal[0]+dz*mask.normal[1]-mask.centre+mask_lateral/2};
    };
    const double reach=half+mask_cell;
    for(uint32_t i=1;i<used;++i) {
        const auto a=local(i-1),b=local(i);
        const double ux=b[0]-a[0],uy=b[1]-a[1],length2=ux*ux+uy*uy;
        const int c0=std::max(0,int(std::floor((std::min(a[0],b[0])-reach)/mask_cell)));
        const int c1=std::min(mask_width-1,int(std::floor((std::max(a[0],b[0])+reach)/mask_cell)));
        const int r0=std::max(0,int(std::floor((std::min(a[1],b[1])-reach)/mask_cell)));
        const int r1=std::min(mask_height-1,int(std::floor((std::max(a[1],b[1])+reach)/mask_cell)));
        for(int r=r0;r<=r1;++r) for(int c=c0;c<=c1;++c) {
            const double px=(c+.5)*mask_cell-a[0],py=(r+.5)*mask_cell-a[1];
            const double t=length2>0?std::clamp((px*ux+py*uy)/length2,0.0,1.0):0.0;
            const double d=std::hypot(px-t*ux,py-t*uy);
            auto& best=distance[size_t(r)*mask_width+c];
            if(d<best) {best=float(d);arc[size_t(r)*mask_width+c]=float(s[i-1]+t*std::sqrt(length2));}
        }
    }
    for(size_t i=0;i<distance.size();++i) {
        const double coverage=1-smoothstep(half-edge_m,half,distance[i]);
        const double fade=1-smoothstep(fade_start,fade_end,arc[i]);
        mask.texels[2*i]=uint8_t(std::lround(255*std::clamp(coverage*fade,0.0,1.0)));
        mask.texels[2*i+1]=uint8_t(std::lround(255*std::clamp(arc[i]/mask_length,0.0,1.0)));
    }
    return mask;
}

using Matrix4=std::array<double,16>; // row-major, column vectors: y = M x
inline Matrix4 multiply(const Matrix4& a,const Matrix4& b) {
    Matrix4 r{};
    for(int i=0;i<4;++i) for(int j=0;j<4;++j) for(int k=0;k<4;++k) r[4*i+j]+=a[4*i+k]*b[4*k+j];
    return r;
}
inline std::optional<Matrix4> inverse(const Matrix4& m) {
    // Gauss-Jordan with partial pivoting.
    std::array<double,32> a{};
    for(int i=0;i<4;++i) for(int j=0;j<4;++j) {a[8*i+j]=m[4*i+j];a[8*i+4+j]=i==j;}
    for(int c=0;c<4;++c) {
        int p=c;for(int r=c+1;r<4;++r) if(std::abs(a[8*r+c])>std::abs(a[8*p+c])) p=r;
        if(!(std::abs(a[8*p+c])>1e-12)) return std::nullopt;
        if(p!=c) for(int j=0;j<8;++j) std::swap(a[8*p+j],a[8*c+j]);
        const double d=a[8*c+c];for(int j=0;j<8;++j) a[8*c+j]/=d;
        for(int r=0;r<4;++r) if(r!=c) {const double f=a[8*r+c];for(int j=0;j<8;++j) a[8*r+j]-=f*a[8*c+j];}
    }
    Matrix4 r{};for(int i=0;i<4;++i) for(int j=0;j<4;++j) r[4*i+j]=a[8*i+4+j];
    for(double x:r) if(!std::isfinite(x)) return std::nullopt;
    return r;
}
// The pass camera as captured at compilation (render_sample.hpp): eye = R (w - o);
// clip = C P eye, C the observed DX11 correction (reversed Z, [-1,1] -> [1,0]);
// viewport depth = near + ndc_z (far - near). The same reconstruction as
// gpu_pack.cpp and otpy reconstruction.
struct Camera {
    Matrix4 clip_from_eye{},eye_from_clip{};
    std::array<double,9> eye_from_world{},world_from_eye{};
    std::array<double,3> origin{};
    double near_depth=0,far_depth=1;
    static std::optional<Camera> make(const std::array<float,16>& projection,const std::array<float,16>& rotation,
                                      const std::array<double,3>& origin,const std::array<float,2>& depth) {
        Camera c;Matrix4 p{};for(int i=0;i<16;++i) p[i]=projection[i];
        const Matrix4 correction{1,0,0,0, 0,1,0,0, 0,0,-.5,.5, 0,0,0,1};
        c.clip_from_eye=multiply(correction,p);
        const auto inverse_clip=inverse(c.clip_from_eye);
        Matrix4 r{};for(int i=0;i<3;++i) for(int j=0;j<3;++j) {r[4*i+j]=rotation[4*i+j];c.eye_from_world[3*i+j]=rotation[4*i+j];}
        r[15]=1;
        const auto inverse_rotation=inverse(r);
        if(!inverse_clip || !inverse_rotation || !(depth[1]>depth[0])) return std::nullopt;
        c.eye_from_clip=*inverse_clip;
        for(int i=0;i<3;++i) for(int j=0;j<3;++j) c.world_from_eye[3*i+j]=(*inverse_rotation)[4*i+j];
        c.origin=origin;c.near_depth=depth[0];c.far_depth=depth[1];
        return c;
    }
    std::array<double,3> eye(const std::array<double,3>& world) const {
        const double d[3]{world[0]-origin[0],world[1]-origin[1],world[2]-origin[2]};
        std::array<double,3> e{};
        for(int i=0;i<3;++i) for(int j=0;j<3;++j) e[i]+=eye_from_world[3*i+j]*d[j];
        return e;
    }
    std::array<double,4> clip(const std::array<double,3>& e) const {
        std::array<double,4> c{};const double v[4]{e[0],e[1],e[2],1};
        for(int i=0;i<4;++i) for(int j=0;j<4;++j) c[i]+=clip_from_eye[4*i+j]*v[j];
        return c;
    }
    // Pixel (x right, y down) and viewport depth of a world point in front of the camera.
    std::optional<std::array<double,3>> pixel(const std::array<double,3>& world,double width,double height) const {
        const auto c=clip(eye(world));
        if(!(c[3]>1e-6)) return std::nullopt;
        return std::array<double,3>{(c[0]/c[3]+1)/2*width,(1-c[1]/c[3])/2*height,near_depth+c[2]/c[3]*(far_depth-near_depth)};
    }
    // Inverse of pixel(): the world point on the depth buffer sample.
    std::array<double,3> unproject(double x,double y,double depth,double width,double height) const {
        const double v[4]{2*(x/width)-1,1-2*(y/height),(depth-near_depth)/(far_depth-near_depth),1};
        double h[4]{};for(int i=0;i<4;++i) for(int j=0;j<4;++j) h[i]+=eye_from_clip[4*i+j]*v[j];
        std::array<double,3> w=origin;
        for(int i=0;i<3;++i) for(int j=0;j<3;++j) w[i]+=world_from_eye[3*i+j]*h[j]/h[3];
        return w;
    }
};
// Screen rectangle (NDC x0,y0,x1,y1; y up) covering the path ribbon, its height
// tolerance and the part in front of the near plane. nullopt: nothing visible.
inline std::optional<std::array<double,4>> path_bounds(const Camera& camera,const double (*points)[2],uint32_t count,
        double ground_y,double half_width,double z_tolerance,double slope) {
    std::array<double,4> box{1e30,1e30,-1e30,-1e30};bool any=false;
    const auto add=[&](const std::array<double,3>& e) {
        const auto c=camera.clip(e);
        const double x=std::clamp(c[0]/c[3],-1.0,1.0),y=std::clamp(c[1]/c[3],-1.0,1.0);
        box={std::min(box[0],x),std::min(box[1],y),std::max(box[2],x),std::max(box[3],y)};any=true;
    };
    constexpr double near_plane=.05; // eye -Z is forward
    // At most ~64 points (8 corners each): the box only limits rasterization.
    const uint32_t step=std::max<uint32_t>(1,count/64);
    const double base=std::isfinite(ground_y)?ground_y:camera.origin[1];
    const auto corners=[&](uint32_t i) {
        const double dx=points[i][0]-camera.origin[0],dz=points[i][1]-camera.origin[2];
        const double rise=z_tolerance+std::hypot(dx,dz)*slope+half_width;
        for(double sx:{-half_width,half_width}) for(double sz:{-half_width,half_width}) for(double sy:{-rise,rise}) {
            const auto e=camera.eye({points[i][0]+sx,base+sy,points[i][1]+sz});
            if(e[2]<-near_plane) {add(e);continue;}
            // Behind: clamp to the near plane on the way to the next sampled point.
            const auto j=i+step<count?i+step:count-1;
            if(j==i) continue;
            const auto f=camera.eye({points[j][0]+sx,base+sy,points[j][1]+sz});
            if(f[2]<-near_plane) {
                const double t=(-near_plane-f[2])/(e[2]-f[2]);
                add({f[0]+t*(e[0]-f[0]),f[1]+t*(e[1]-f[1]),-near_plane});
            }
        }
    };
    for(uint32_t i=0;i<count;i+=step) corners(i);
    if(count && (count-1)%step) corners(count-1);
    if(!any || box[2]<=box[0] || box[3]<=box[1]) return std::nullopt;
    return box;
}
}
