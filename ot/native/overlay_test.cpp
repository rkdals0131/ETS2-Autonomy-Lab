// Game-free checks of the HUD decal pieces (docs/24): the BEV mask rasterizer,
// its world<->mask affine, the OT_Overlay seqlock under a concurrent writer,
// and the camera projection/reconstruction used for the decal and its bounds.
#include "ipc_layout.hpp"
#include "overlay_mask.hpp"
#include <atomic>
#include <cstdio>
#include <memory>
#include <numbers>
#include <thread>

using namespace ot;
namespace {
int failures=0;
void check(bool ok,const char* what) {std::printf("%s %s\n",ok?"PASS":"FAIL",what);if(!ok) ++failures;}
bool close_to(double a,double b,double tolerance) {return std::abs(a-b)<=tolerance;}
uint8_t texel(const overlay::Mask& m,double x,double z,int channel) {
    const auto uv=m.uv(x,z);
    const int c=int(std::floor(uv[0]*overlay::mask_width)),r=int(std::floor(uv[1]*overlay::mask_height));
    if(c<0 || r<0 || c>=overlay::mask_width || r>=overlay::mask_height) return 0;
    return m.texels[(size_t(r)*overlay::mask_width+c)*2+channel];
}

void mask_straight() {
    // A straight path heading -Z (SCS north) from far-away coordinates.
    double points[150][2];
    for(int i=0;i<150;++i) {points[i][0]=-12995.5;points[i][1]=837.5-1.5*i;}
    const auto m=overlay::rasterize(points,150,3.5);
    check(close_to(m.axis[0],0,1e-9) && close_to(m.axis[1],-1,1e-9),"straight mask axis follows the path");
    check(m.points_used<150 && m.length>=overlay::mask_length-overlay::mask_back,"points beyond the mask length are not used");
    check(texel(m,-12995.5,837.5-20,0)==255,"centre line at 20 m is fully covered");
    check(texel(m,-12995.5+1.0,837.5-20,0)==255,"1 m off the centre is inside a 3.5 m ribbon");
    check(texel(m,-12995.5+1.75+.3,837.5-20,0)==0,"0.3 m outside the edge is empty");
    const auto edge=texel(m,-12995.5+1.75-.075,837.5-20,0);
    check(edge>0 && edge<255,"the 0.15 m edge band is partially covered");
    check(texel(m,-12995.5,837.5-100,0)>texel(m,-12995.5,837.5-115,0),"coverage fades between 80 and 120 m");
    check(texel(m,-12995.5,837.5-130,0)==0,"nothing after the fade end");
    const auto g20=texel(m,-12995.5,837.5-20,1),g60=texel(m,-12995.5,837.5-60,1);
    check(close_to(g20/255.0*overlay::mask_length,20,1) && close_to(g60/255.0*overlay::mask_length,60,1),"G holds the arc length / 160 m");
    // Affine and texel centres are inverse maps.
    bool round_trip=true;
    for(int c:{0,17,320,639}) for(int r:{0,40,127}) {
        const auto w=m.texel_world(c,r);const auto uv=m.uv(w[0],w[1]);
        round_trip=round_trip && close_to(uv[0],(c+.5)/overlay::mask_width,1e-9) && close_to(uv[1],(r+.5)/overlay::mask_height,1e-9);
    }
    check(round_trip,"world->uv affine inverts texel centres");
    const auto a=m.affine();
    const auto direct=m.uv(-12990.0,800.0);
    check(close_to(a[0]*(-12990.0-m.origin[0])+a[1]*(800.0-m.origin[1])+a[2],direct[0],1e-12),"affine coefficients match uv()");
}
void mask_curve() {
    // A 500 m radius curve: the chord axis and band centre keep 120 m inside 32 m.
    double points[200][2];const double radius=500;
    for(int i=0;i<200;++i) {const double t=i*1.5/radius;points[i][0]=1000+radius*(1-std::cos(t));points[i][1]=2000-radius*std::sin(t);}
    const auto m=overlay::rasterize(points,200,3.5);
    check(!m.clipped,"500 m radius curve fits the band");
    bool covered=true;
    for(double s=0;s<=78;s+=6) {
        const double t=s/radius;covered=covered && texel(m,1000+radius*(1-std::cos(t)),2000-radius*std::sin(t),0)==255;
    }
    check(covered,"centre line covered along the curve up to 78 m");
    double tight[200][2];
    for(int i=0;i<200;++i) {const double t=i*1.5/60;tight[i][0]=60*(1-std::cos(t));tight[i][1]=-60*std::sin(t);}
    check(overlay::rasterize(tight,200,3.5).clipped,"a 60 m radius U-turn is reported as clipped");
    double same[3][2]{{5,5},{5,5},{5,5}};
    check(overlay::rasterize(same,3,3.5).points_used==0,"a degenerate path yields an empty mask");
}
void seqlock() {
    auto block=std::make_unique<OverlayBlock>();overlay_initialize(block.get());
    check(overlay_header_valid(block.get()),"initialized header is valid");
    auto out=std::make_unique<OverlayContent>();LONG64 generation{};
    check(overlay_read(block.get(),-1,*out,generation)==1 && generation==0 && !out->valid,"fresh block reads as invalid generation 0");
    check(overlay_read(block.get(),0,*out,generation)==0,"unchanged generation is not copied again");
    auto in=std::make_unique<OverlayContent>();
    in->point_count=3;in->stamp_us=123;in->ttl_ms=500;in->width_m=3.5f;in->valid=true;in->ground_y=41.5;
    for(int i=0;i<3;++i) {in->points[i][0]=-1e5+i;in->points[i][1]=1e5-i;}
    overlay_write(block.get(),*in);
    check(overlay_read(block.get(),0,*out,generation)==1 && generation==2 && out->point_count==3 && out->ground_y==41.5 &&
          out->points[2][0]==-1e5+2 && out->valid,"one write is read back at generation 2");
    block->generation|=1;
    check(overlay_read(block.get(),2,*out,generation)==-1,"an odd generation is skipped");
    block->generation+=1;block->point_count=999;
    check(overlay_read(block.get(),2,*out,generation)==-2,"too many points are rejected");
    // A writer streams paths whose every point encodes its generation; the
    // reader must only ever accept self-consistent copies.
    overlay_initialize(block.get());
    std::atomic<bool> done{false};
    std::thread writer([&] {
        auto c=std::make_unique<OverlayContent>();c->valid=true;c->width_m=3;
        for(uint32_t n=1;n<=20000;++n) {
            c->point_count=1+n%overlay_max_points;c->stamp_us=n;
            for(uint32_t i=0;i<c->point_count;++i) {c->points[i][0]=n;c->points[i][1]=-double(n);}
            overlay_write(block.get(),*c);
        }
        done=true;
    });
    uint64_t accepted=0,torn=0;bool consistent=true;LONG64 seen=-1;
    while(!done || accepted==0) {
        const auto r=overlay_read(block.get(),seen,*out,generation);
        if(r==-1) {++torn;continue;}
        if(r!=1) continue;
        seen=generation;++accepted;
        if(!out->valid) continue;
        const auto n=out->stamp_us;
        consistent=consistent && out->point_count==1+n%overlay_max_points;
        for(uint32_t i=0;i<out->point_count;++i) consistent=consistent && out->points[i][0]==double(n) && out->points[i][1]==-double(n);
    }
    writer.join();
    std::printf("     seqlock: %llu accepted, %llu torn reads retried\n",(unsigned long long)accepted,(unsigned long long)torn);
    check(consistent && accepted>0,"concurrent reads never accept a torn path");
}
void camera() {
    // A GL-style projection like the captured main pass (P[10], P[11] of the exterior).
    const double fov=std::numbers::pi/3,aspect=16.0/9,n=.4,f=2000,t=1/std::tan(fov/2);
    std::array<float,16> projection{float(t/aspect),0,0,0, 0,float(t),0,0, 0,0,float(-(f+n)/(f-n)),float(-2*f*n/(f-n)), 0,0,-1,0};
    // Yaw 30 degrees and a small pitch; rows map world into eye.
    const double yaw=std::numbers::pi/6,pitch=-.05;
    const double cy=std::cos(yaw),sy=std::sin(yaw),cp=std::cos(pitch),sp=std::sin(pitch);
    // eye = Rx(pitch) Ry(yaw) world
    const double ry[9]{cy,0,-sy, 0,1,0, sy,0,cy},rx[9]{1,0,0, 0,cp,-sp, 0,sp,cp};
    std::array<float,16> rotation{};
    for(int i=0;i<3;++i) for(int j=0;j<3;++j) {double v=0;for(int k=0;k<3;++k) v+=rx[3*i+k]*ry[3*k+j];rotation[4*i+j]=float(v);}
    rotation[15]=1;
    const std::array<double,3> origin{-12990.25,45.5,830.75};
    const auto c=overlay::Camera::make(projection,rotation,origin,{.01f,.9f});
    check(c.has_value(),"camera from projection, rotation and depth range");
    bool round=true;double worst=0;
    for(const auto& w:std::array<std::array<double,3>,3>{{{origin[0]-10,43.0,origin[2]-40},{origin[0]+5,44.0,origin[2]-120},{origin[0]-30,42.0,origin[2]-15}}}) {
        const auto p=c->pixel(w,1920,1080);
        if(!p) {round=false;continue;}
        const auto back=c->unproject((*p)[0],(*p)[1],(*p)[2],1920,1080);
        for(int i=0;i<3;++i) worst=std::max(worst,std::abs(back[i]-w[i]));
        round=round && (*p)[2]>=.01 && (*p)[2]<=.9;
    }
    std::printf("     camera round trip worst error %.2e m\n",worst);
    check(round && worst<1e-6,"pixel() and unproject() invert each other inside [near, far]");
    check(!c->pixel({origin[0]+20*sy,45.5,origin[2]+20*cy},1920,1080),"a point behind the camera has no pixel");
    // Bounds: a path ahead is on screen; behind starts are clamped at the near plane.
    double path[60][2];
    for(int i=0;i<60;++i) {path[i][0]=origin[0]+(i-5)*1.5*-sy;path[i][1]=origin[2]+(i-5)*1.5*-cy;}
    const auto box=overlay::path_bounds(*c,path,60,origin[1]-2.5,1.9,1,.14);
    check(box.has_value() && (*box)[0]<(*box)[2] && (*box)[1]<(*box)[3] && (*box)[1]>=-1 && (*box)[3]<=1,"path ahead has a screen box inside NDC");
    check(box && (*box)[1]==-1,"the start under the camera reaches the screen bottom");
    double behind[10][2];
    for(int i=0;i<10;++i) {behind[i][0]=origin[0]+(i+5)*sy;behind[i][1]=origin[2]+(i+5)*cy;}
    check(!overlay::path_bounds(*c,behind,10,origin[1]-2.5,1.9,1,.14),"a path entirely behind has no box");
}
}
int main() {
    mask_straight();mask_curve();seqlock();camera();
    std::printf("%s\n",failures?"FAILED":"ALL PASSED");
    return failures?1:0;
}
