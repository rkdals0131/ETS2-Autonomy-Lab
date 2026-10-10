#include "ets2_sensor_degradation/models.hpp"
#include <gtest/gtest.h>
#include <set>
#include <vector>

using namespace ets2_sensor_degradation;

namespace {
struct Moments {double mean=0,variance=0;};
Moments moments(const std::vector<double>& values) {
    Moments m;for(const double v:values) m.mean+=v;m.mean/=values.size();
    for(const double v:values) m.variance+=(v-m.mean)*(v-m.mean);m.variance/=values.size()-1;
    return m;
}
// The plan's acceptance band: sample variance within +/-10% of the configured value.
void expect_variance(const std::vector<double>& values,double expected) {
    const auto m=moments(values);
    EXPECT_NEAR(m.variance/expected,1.0,0.1) << "variance " << m.variance << " expected " << expected;
}
}

TEST(Rng,SameSeedSameSequenceAndIndependentStreams) {
    Rng a(stream_seed(7,"imu")),b(stream_seed(7,"imu")),c(stream_seed(7,"gnss")),d(stream_seed(8,"imu"));
    std::vector<double> first,second,other,reseeded;
    for(int i=0;i<1000;++i) {first.push_back(a.normal());second.push_back(b.normal());other.push_back(c.normal());reseeded.push_back(d.normal());}
    EXPECT_EQ(first,second);
    EXPECT_NE(first,other);
    EXPECT_NE(first,reseeded);
}

TEST(Rng,StandardNormalMoments) {
    Rng rng(1);std::vector<double> values(400000);for(auto& v:values) v=rng.normal();
    const auto m=moments(values);
    EXPECT_NEAR(m.mean,0,0.01);
    EXPECT_NEAR(m.variance,1,0.01);
}

TEST(GaussMarkov,StationaryVariance) {
    Rng rng(3);GaussMarkov process(1.5,60);std::vector<double> values;
    for(int i=0;i<200000;++i) values.push_back(process.step(rng,60)); // dt=tau keeps samples weakly correlated
    expect_variance(values,1.5*1.5);
}

TEST(Imu,WhiteNoiseMatchesDensityAndIsReproducible) {
    ImuParameters p;p.gyro_bias_initial=p.gyro_bias_walk=p.accel_bias_initial=p.accel_bias_walk=p.misalignment_rad=0;
    ImuModel model(p,11),again(p,11);
    const double dt=0.01;std::vector<double> gyro,accel;
    for(int i=0;i<50000;++i) {
        const auto out=model.apply({0,0,0},{0,0,9.81},i*dt);
        EXPECT_EQ(out.gyro,again.apply({0,0,0},{0,0,9.81},i*dt).gyro);
        gyro.push_back(out.gyro[2]);accel.push_back(out.accel[0]);
        if(i) {EXPECT_NEAR(out.gyro_variance/(p.gyro_noise_density*p.gyro_noise_density/dt),1.0,1e-6);} // stamps differ by dt up to rounding
    }
    expect_variance(gyro,p.gyro_noise_density*p.gyro_noise_density/dt);
    expect_variance(accel,p.accel_noise_density*p.accel_noise_density/dt);
}

TEST(Imu,MisalignmentAndBiasAreFixedPerSeed) {
    ImuParameters p;p.gyro_noise_density=p.accel_noise_density=p.gyro_bias_walk=p.accel_bias_walk=0;
    ImuModel model(p,5);
    const auto first=model.apply({0,0,1},{0,0,9.81},0),later=model.apply({0,0,1},{0,0,9.81},1);
    EXPECT_EQ(first.gyro,later.gyro);
    EXPECT_NEAR(first.accel[2],9.81,0.1); // small rotation plus ~0.02 m/s^2 bias
    EXPECT_GT(std::abs(first.accel[0]),1e-6);
}

TEST(Gnss,HorizontalErrorInMetersAndOutage) {
    GnssParameters p;p.correlation_time_s=0.1; // nearly independent 10 Hz samples
    GnssModel model(p,21);std::vector<double> east,north,up;
    constexpr double lat=37.5,lon=127,alt=50,a=6378137.,e2=6.6943799901413165e-3;
    const double s=std::sin(lat*std::numbers::pi/180),w=1-e2*s*s,n=a/std::sqrt(w),m=a*(1-e2)/(w*std::sqrt(w));
    for(int i=0;i<100000;++i) {
        const auto fix=model.apply(lat,lon,alt,i*0.1);ASSERT_TRUE(fix.fix);
        north.push_back((fix.latitude_deg-lat)*std::numbers::pi/180*(m+alt));
        east.push_back((fix.longitude_deg-lon)*std::numbers::pi/180*(n+alt)*std::cos(lat*std::numbers::pi/180));
        up.push_back(fix.altitude_m-alt);
    }
    expect_variance(east,p.horizontal_sigma_m*p.horizontal_sigma_m);
    expect_variance(north,p.horizontal_sigma_m*p.horizontal_sigma_m);
    expect_variance(up,p.vertical_sigma_m*p.vertical_sigma_m);

    GnssParameters outage;outage.outage_rate_hz=1000;outage.outage_duration_s=2;
    GnssModel blocked(outage,1);
    EXPECT_FALSE(blocked.apply(lat,lon,alt,0.0).fix);
    EXPECT_FALSE(blocked.apply(lat,lon,alt,1.9).fix);
}

TEST(Wheels,RadiusScaleIsPerIndexAndQuantizationHolds) {
    WheelParameters p;WheelModel a(p,9),b(p,9);
    EXPECT_EQ(a.radius(2,0.5),b.radius(2,0.5));
    EXPECT_NE(a.radius(2,0.5),a.radius(3,0.5));
    for(int i=0;i<1000;++i) {
        const double value=a.angular(20.0);
        EXPECT_NEAR(std::remainder(value,p.angular_step),0,1e-9);
    }
    WheelParameters smooth;smooth.angular_step=0;WheelModel noisy(smooth,4);std::vector<double> values;
    for(int i=0;i<100000;++i) values.push_back(noisy.angular(20.0));
    expect_variance(values,smooth.angular_noise_std*smooth.angular_noise_std);
}

TEST(Lidar,RangeNoiseAndMissRate) {
    LidarParameters p;LidarModel model(p,2,"L_F");std::vector<double> ranges;int misses=0;
    const int total=200000;
    for(int i=0;i<total;++i) {const auto r=model.range(40.0);if(r) ranges.push_back(*r);else ++misses;}
    expect_variance(ranges,p.range_sigma_m*p.range_sigma_m);
    EXPECT_NEAR(static_cast<double>(misses)/total/p.miss_probability,1.0,0.1);
}

TEST(DelayLine,LatencyAndDrops) {
    DelayLine<int> line(0.1,0.0,1,"imu");
    line.push(1.00,1);line.push(1.05,2);
    std::vector<int> out;auto take=[&](int v){out.push_back(v);};
    EXPECT_EQ(line.release(1.099,take),0u);
    EXPECT_EQ(line.release(1.100,take),1u);
    EXPECT_EQ(line.release(2.0,take),1u);
    EXPECT_EQ(out,(std::vector<int>{1,2}));

    DelayLine<int> lossy(0,0.2,1,"gnss");int kept=0;const int total=100000;
    for(int i=0;i<total;++i) kept+=lossy.push(i,i);
    EXPECT_NEAR(1-static_cast<double>(kept)/total,0.2,0.02);
}
