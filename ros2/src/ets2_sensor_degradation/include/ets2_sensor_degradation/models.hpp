#pragma once
// Sensor error models without ROS types, so they can be unit tested and reused
// on bag replay. Randomness comes only from Rng: splitmix64 plus Box-Muller is
// reproducible for a seed on every standard library, unlike std:: distributions.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ets2_sensor_degradation {
using Vec3=std::array<double,3>;

class Rng {
public:
    explicit Rng(uint64_t seed):state_(seed) {}
    uint64_t next() {
        uint64_t z=(state_+=0x9e3779b97f4a7c15ull);
        z=(z^(z>>30))*0xbf58476d1ce4e5b9ull;z=(z^(z>>27))*0x94d049bb133111ebull;
        return z^(z>>31);
    }
    double uniform() {return static_cast<double>(next()>>11)*0x1.0p-53;} // [0,1)
    double normal() {
        if(spare_) {const double value=*spare_;spare_.reset();return value;}
        double u=0;while(u<=0) u=uniform();
        const double radius=std::sqrt(-2*std::log(u)),angle=2*std::numbers::pi*uniform();
        spare_=radius*std::sin(angle);
        return radius*std::cos(angle);
    }
    bool chance(double probability) {return uniform()<probability;}
private:
    uint64_t state_;
    std::optional<double> spare_;
};

// One independent stream per sensor and purpose (FNV-1a of the name), so adding a
// topic or reordering messages never shifts another sensor's noise sequence.
inline uint64_t stream_seed(uint64_t seed,std::string_view name) {
    uint64_t hash=1469598103934665603ull;
    for(const char c:name) {hash^=static_cast<uint8_t>(c);hash*=1099511628211ull;}
    return Rng(seed^hash).next();
}

// Sample interval from consecutive stamps; pauses and resets fall back to nominal.
inline double sample_interval(std::optional<double>& previous,double stamp_s,double nominal_s) {
    const double dt=previous?stamp_s-*previous:nominal_s;
    previous=stamp_s;
    return dt>0 && dt<1?dt:nominal_s;
}

// First-order Gauss-Markov process with stationary standard deviation sigma.
class GaussMarkov {
public:
    GaussMarkov(double sigma,double correlation_time_s):sigma_(sigma),tau_(correlation_time_s) {}
    double step(Rng& rng,double dt) {
        if(!started_) {value_=sigma_*rng.normal();started_=true;return value_;}
        const double a=std::exp(-dt/tau_);
        value_=a*value_+sigma_*std::sqrt(1-a*a)*rng.normal();
        return value_;
    }
private:
    double sigma_,tau_,value_=0;
    bool started_=false;
};

struct ImuParameters {
    double gyro_noise_density=8.7e-5;    // rad/s/sqrt(Hz)
    double gyro_bias_initial=1e-3;       // rad/s, 1 sigma turn-on bias
    double gyro_bias_walk=1e-5;          // rad/s^2/sqrt(Hz)
    double accel_noise_density=1e-3;     // m/s^2/sqrt(Hz)
    double accel_bias_initial=2e-2;      // m/s^2
    double accel_bias_walk=1e-4;         // m/s^3/sqrt(Hz)
    double misalignment_rad=1.7e-3;      // 1 sigma per axis, fixed per seed
    double nominal_rate_hz=60;
};
struct ImuSample {Vec3 gyro,accel;double gyro_variance,accel_variance;};

class ImuModel {
public:
    ImuModel(const ImuParameters& p,uint64_t seed):p_(p),noise_(stream_seed(seed,"imu.noise")) {
        Rng fixed(stream_seed(seed,"imu.fixed"));
        const Vec3 angle{p.misalignment_rad*fixed.normal(),p.misalignment_rad*fixed.normal(),p.misalignment_rad*fixed.normal()};
        rotation_=rodrigues(angle);
        for(auto& b:gyro_bias_) b=p.gyro_bias_initial*fixed.normal();
        for(auto& b:accel_bias_) b=p.accel_bias_initial*fixed.normal();
    }
    ImuSample apply(const Vec3& gyro,const Vec3& accel,double stamp_s) {
        const double dt=sample_interval(previous_,stamp_s,1/p_.nominal_rate_hz);
        const double gyro_sigma=p_.gyro_noise_density/std::sqrt(dt),accel_sigma=p_.accel_noise_density/std::sqrt(dt);
        ImuSample out{rotate(gyro),rotate(accel),gyro_sigma*gyro_sigma,accel_sigma*accel_sigma};
        for(size_t i=0;i<3;++i) {
            gyro_bias_[i]+=p_.gyro_bias_walk*std::sqrt(dt)*noise_.normal();
            accel_bias_[i]+=p_.accel_bias_walk*std::sqrt(dt)*noise_.normal();
            out.gyro[i]+=gyro_bias_[i]+gyro_sigma*noise_.normal();
            out.accel[i]+=accel_bias_[i]+accel_sigma*noise_.normal();
        }
        return out;
    }
private:
    static std::array<double,9> rodrigues(const Vec3& v) {
        const double angle=std::hypot(v[0],v[1],v[2]);
        if(angle<1e-15) return {1,0,0,0,1,0,0,0,1};
        const double x=v[0]/angle,y=v[1]/angle,z=v[2]/angle,c=std::cos(angle),s=std::sin(angle),t=1-c;
        return {t*x*x+c,t*x*y-s*z,t*x*z+s*y, t*x*y+s*z,t*y*y+c,t*y*z-s*x, t*x*z-s*y,t*y*z+s*x,t*z*z+c};
    }
    Vec3 rotate(const Vec3& v) const {
        Vec3 r{};
        for(size_t i=0;i<3;++i) for(size_t j=0;j<3;++j) r[i]+=rotation_[3*i+j]*v[j];
        return r;
    }
    ImuParameters p_;
    Rng noise_;
    std::array<double,9> rotation_{};
    Vec3 gyro_bias_{},accel_bias_{};
    std::optional<double> previous_;
};

struct GnssParameters {
    double horizontal_sigma_m=1.5;
    double vertical_sigma_m=3.0;
    double correlation_time_s=60;
    double outage_rate_hz=0;      // expected outage starts per second
    double outage_duration_s=10;
    double nominal_rate_hz=10;
};
struct GnssFix {double latitude_deg,longitude_deg,altitude_m;bool fix;double horizontal_variance,vertical_variance;};

class GnssModel {
public:
    GnssModel(const GnssParameters& p,uint64_t seed):p_(p),rng_(stream_seed(seed,"gnss")),
        east_(p.horizontal_sigma_m,p.correlation_time_s),north_(p.horizontal_sigma_m,p.correlation_time_s),
        up_(p.vertical_sigma_m,p.correlation_time_s) {}
    GnssFix apply(double latitude_deg,double longitude_deg,double altitude_m,double stamp_s) {
        const double dt=sample_interval(previous_,stamp_s,1/p_.nominal_rate_hz);
        const double e=east_.step(rng_,dt),n=north_.step(rng_,dt),u=up_.step(rng_,dt);
        if(stamp_s>=outage_end_ && rng_.chance(p_.outage_rate_hz*dt)) outage_end_=stamp_s+p_.outage_duration_s;
        const double nan=std::numeric_limits<double>::quiet_NaN();
        if(stamp_s<outage_end_) return {nan,nan,nan,false,0,0};
        // WGS84 meridian (m) and prime vertical (nprime) radii at the true latitude.
        constexpr double a=6378137.,e2=6.6943799901413165e-3;
        const double lat=latitude_deg*std::numbers::pi/180,s=std::sin(lat),w=1-e2*s*s;
        const double nprime=a/std::sqrt(w),m=a*(1-e2)/(w*std::sqrt(w));
        return {latitude_deg+n/(m+altitude_m)*180/std::numbers::pi,
                longitude_deg+e/((nprime+altitude_m)*std::cos(lat))*180/std::numbers::pi,
                altitude_m+u,true,p_.horizontal_sigma_m*p_.horizontal_sigma_m,p_.vertical_sigma_m*p_.vertical_sigma_m};
    }
private:
    GnssParameters p_;
    Rng rng_;
    GaussMarkov east_,north_,up_;
    std::optional<double> previous_;
    double outage_end_=-std::numeric_limits<double>::infinity();
};

struct WheelParameters {
    double angular_noise_std=0.05;   // rad/s
    double angular_step=0.01;        // rad/s quantization; 0 disables
    double steering_noise_std=1e-3;  // rad
    double radius_scale_std=5e-3;    // relative, fixed per wheel index
};

class WheelModel {
public:
    WheelModel(const WheelParameters& p,uint64_t seed):p_(p),seed_(seed),noise_(stream_seed(seed,"wheels.noise")) {}
    double angular(double radps) {
        const double value=radps+p_.angular_noise_std*noise_.normal();
        return p_.angular_step>0?std::round(value/p_.angular_step)*p_.angular_step:value;
    }
    double steering(double rad) {return rad+p_.steering_noise_std*noise_.normal();}
    // Reported radius: a fixed per-index scale error that does not depend on message order.
    double radius(uint32_t index,double true_radius_m) const {
        Rng fixed(stream_seed(seed_,"wheels.radius."+std::to_string(index)));
        return true_radius_m*(1+p_.radius_scale_std*fixed.normal());
    }
private:
    WheelParameters p_;
    uint64_t seed_;
    Rng noise_;
};

struct LidarParameters {
    double range_sigma_m=0.02;
    double range_sigma_relative=0.0;
    double miss_probability=0.01;
};

class LidarModel {
public:
    LidarModel(const LidarParameters& p,uint64_t seed,std::string_view name):p_(p),rng_(stream_seed(seed,std::string("lidar.")+std::string(name))) {}
    // A returned range, or nullopt when the return is dropped.
    std::optional<double> range(double true_range_m) {
        if(rng_.chance(p_.miss_probability)) return std::nullopt;
        const double sigma=p_.range_sigma_m+p_.range_sigma_relative*true_range_m;
        return std::max(0.0,true_range_m+sigma*rng_.normal());
    }
private:
    LidarParameters p_;
    Rng rng_;
};

// Holds messages until stamp + latency on the consumer clock; drops are decided
// on arrival so the drop pattern does not depend on timer jitter.
template<class T> class DelayLine {
public:
    DelayLine(double latency_s,double drop_probability,uint64_t seed,std::string_view name):
        latency_(latency_s),drop_(drop_probability),rng_(stream_seed(seed,std::string("delay.")+std::string(name))) {}
    bool push(double stamp_s,T value) {
        if(rng_.chance(drop_)) return false;
        queue_.emplace_back(stamp_s+latency_,std::move(value));
        return true;
    }
    template<class F> size_t release(double now_s,F&& publish) {
        size_t count=0;
        while(!queue_.empty() && queue_.front().first<=now_s) {publish(std::move(queue_.front().second));queue_.pop_front();++count;}
        return count;
    }
    size_t size() const {return queue_.size();}
private:
    double latency_,drop_;
    Rng rng_;
    std::deque<std::pair<double,T>> queue_;
};
}
