// Republishes ideal /ets2 sensors as /sensing topics with seeded noise, latency and
// drops. Stamps stay at measurement time; latency delays publication on the node
// clock (/clock when use_sim_time). Inputs are subscribed only while an output has
// a subscriber, because the relay turns subscriptions into render demand.
#include "ets2_sensor_degradation/models.hpp"
#include <ets2_msgs/msg/wheel_state.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/nav_sat_fix.hpp>
#include <sensor_msgs/msg/nav_sat_status.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <sensor_msgs/msg/point_field.hpp>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

using namespace std::chrono_literals;
namespace degradation=ets2_sensor_degradation;

namespace {
double seconds(const builtin_interfaces::msg::Time& stamp) {return stamp.sec+stamp.nanosec*1e-9;}

// One /ets2 input and its /sensing output with a delay line.
template<class Message> class Channel {
public:
    using Transform=std::function<void(Message&)>;
    Channel(rclcpp::Node& node,const std::string& name,const std::string& input,const std::string& output,uint64_t seed,Transform transform):
        node_(node),input_(input),transform_(std::move(transform)),
        delay_(node.declare_parameter(name+".latency_ms",0.0)*1e-3,node.declare_parameter(name+".drop_probability",0.0),seed,name),
        publisher_(node.create_publisher<Message>(output,rclcpp::SensorDataQoS())) {}
    // Subscribe while someone listens; unsubscribing lets the relay stop that sensor.
    void reconcile() {
        const bool wanted=publisher_->get_subscription_count()>0;
        if(wanted && !subscription_) subscription_=node_.create_subscription<Message>(input_,rclcpp::SensorDataQoS(),
            [this](typename Message::UniquePtr message) {
                transform_(*message);
                const double stamp=seconds(message->header.stamp);
                if(delay_.push(stamp,std::move(message))) release(node_.now().seconds());
            });
        else if(!wanted && subscription_) subscription_.reset();
    }
    void release(double now_s) {delay_.release(now_s,[this](typename Message::UniquePtr message) {publisher_->publish(std::move(message));});}
private:
    rclcpp::Node& node_;
    std::string input_;
    Transform transform_;
    degradation::DelayLine<typename Message::UniquePtr> delay_;
    typename rclcpp::Publisher<Message>::SharedPtr publisher_;
    typename rclcpp::Subscription<Message>::SharedPtr subscription_;
};

struct CloudLayout {uint32_t x,y,z,range,status;};
std::optional<CloudLayout> layout(const sensor_msgs::msg::PointCloud2& cloud) {
    using sensor_msgs::msg::PointField;
    CloudLayout result{};uint32_t found=0;
    for(const auto& field:cloud.fields) {
        const auto set=[&](const char* name,uint8_t type,uint32_t& offset,uint32_t bit) {
            if(field.name==name && field.datatype==type && field.count==1) {offset=field.offset;found|=bit;}
        };
        set("x",PointField::FLOAT32,result.x,1);set("y",PointField::FLOAT32,result.y,2);set("z",PointField::FLOAT32,result.z,4);
        set("range",PointField::FLOAT32,result.range,8);set("status",PointField::UINT8,result.status,16);
    }
    if(found!=31 || cloud.is_bigendian) return std::nullopt;
    return result;
}
}

class SensorDegradation:public rclcpp::Node {
public:
    SensorDegradation():Node("sensor_degradation") {
        const auto seed=static_cast<uint64_t>(declare_parameter<int64_t>("seed",1));
        degradation::ImuParameters imu;
        imu.gyro_noise_density=declare_parameter("imu.gyro_noise_density",imu.gyro_noise_density);
        imu.gyro_bias_initial=declare_parameter("imu.gyro_bias_initial",imu.gyro_bias_initial);
        imu.gyro_bias_walk=declare_parameter("imu.gyro_bias_walk",imu.gyro_bias_walk);
        imu.accel_noise_density=declare_parameter("imu.accel_noise_density",imu.accel_noise_density);
        imu.accel_bias_initial=declare_parameter("imu.accel_bias_initial",imu.accel_bias_initial);
        imu.accel_bias_walk=declare_parameter("imu.accel_bias_walk",imu.accel_bias_walk);
        imu.misalignment_rad=declare_parameter("imu.misalignment_rad",imu.misalignment_rad);
        degradation::GnssParameters gnss;
        gnss.horizontal_sigma_m=declare_parameter("gnss.horizontal_sigma_m",gnss.horizontal_sigma_m);
        gnss.vertical_sigma_m=declare_parameter("gnss.vertical_sigma_m",gnss.vertical_sigma_m);
        gnss.correlation_time_s=declare_parameter("gnss.correlation_time_s",gnss.correlation_time_s);
        gnss.outage_rate_hz=declare_parameter("gnss.outage_rate_hz",gnss.outage_rate_hz);
        gnss.outage_duration_s=declare_parameter("gnss.outage_duration_s",gnss.outage_duration_s);
        degradation::WheelParameters wheels;
        wheels.angular_noise_std=declare_parameter("wheels.angular_noise_std",wheels.angular_noise_std);
        wheels.angular_step=declare_parameter("wheels.angular_step",wheels.angular_step);
        wheels.steering_noise_std=declare_parameter("wheels.steering_noise_std",wheels.steering_noise_std);
        wheels.radius_scale_std=declare_parameter("wheels.radius_scale_std",wheels.radius_scale_std);
        degradation::LidarParameters lidar;
        lidar.range_sigma_m=declare_parameter("lidar.range_sigma_m",lidar.range_sigma_m);
        lidar.range_sigma_relative=declare_parameter("lidar.range_sigma_relative",lidar.range_sigma_relative);
        lidar.miss_probability=declare_parameter("lidar.miss_probability",lidar.miss_probability);
        const auto lidars=declare_parameter("lidar.names",std::vector<std::string>{"L_F","L_PL","L_PR"});

        imu_model_=std::make_unique<degradation::ImuModel>(imu,seed);
        imu_=std::make_unique<Channel<sensor_msgs::msg::Imu>>(*this,"imu","/ets2/imu/data_raw","/sensing/imu/imu_raw",seed,
            [this](sensor_msgs::msg::Imu& m) {
                const auto& w=m.angular_velocity;const auto& a=m.linear_acceleration;
                const auto out=imu_model_->apply({w.x,w.y,w.z},{a.x,a.y,a.z},seconds(m.header.stamp));
                m.angular_velocity.x=out.gyro[0];m.angular_velocity.y=out.gyro[1];m.angular_velocity.z=out.gyro[2];
                m.linear_acceleration.x=out.accel[0];m.linear_acceleration.y=out.accel[1];m.linear_acceleration.z=out.accel[2];
                m.angular_velocity_covariance={out.gyro_variance,0,0,0,out.gyro_variance,0,0,0,out.gyro_variance};
                m.linear_acceleration_covariance={out.accel_variance,0,0,0,out.accel_variance,0,0,0,out.accel_variance};
            });
        gnss_model_=std::make_unique<degradation::GnssModel>(gnss,seed);
        gnss_=std::make_unique<Channel<sensor_msgs::msg::NavSatFix>>(*this,"gnss","/ets2/gnss/fix","/sensing/gnss/nav_sat_fix",seed,
            [this](sensor_msgs::msg::NavSatFix& m) {
                const auto out=gnss_model_->apply(m.latitude,m.longitude,m.altitude,seconds(m.header.stamp));
                m.latitude=out.latitude_deg;m.longitude=out.longitude_deg;m.altitude=out.altitude_m;
                m.position_covariance={out.horizontal_variance,0,0,0,out.horizontal_variance,0,0,0,out.vertical_variance};
                m.position_covariance_type=out.fix?sensor_msgs::msg::NavSatFix::COVARIANCE_TYPE_DIAGONAL_KNOWN:sensor_msgs::msg::NavSatFix::COVARIANCE_TYPE_UNKNOWN;
                if(!out.fix) m.status.status=sensor_msgs::msg::NavSatStatus::STATUS_NO_FIX;
            });
        wheel_model_=std::make_unique<degradation::WheelModel>(wheels,seed);
        wheels_=std::make_unique<Channel<ets2_msgs::msg::WheelState>>(*this,"wheels","/ets2/wheels/state","/sensing/wheels/state",seed,
            [this](ets2_msgs::msg::WheelState& m) {
                for(size_t i=0;i<m.index.size();++i) {
                    m.angular_velocity_radps[i]=wheel_model_->angular(m.angular_velocity_radps[i]);
                    m.steering_angle_rad[i]=wheel_model_->steering(m.steering_angle_rad[i]);
                    m.radius_m[i]=wheel_model_->radius(m.index[i],m.radius_m[i]);
                }
            });
        for(const auto& name:lidars) {
            auto model=std::make_shared<degradation::LidarModel>(lidar,seed,name);
            lidars_.push_back(std::make_unique<Channel<sensor_msgs::msg::PointCloud2>>(*this,"lidar."+name,
                "/ets2/lidar/"+name+"/points","/sensing/lidar/"+name+"/points",seed,
                [this,model,name](sensor_msgs::msg::PointCloud2& cloud) {
                    const auto fields=layout(cloud);
                    if(!fields) {RCLCPP_ERROR_THROTTLE(get_logger(),*get_clock(),5000,"%s lacks float32 x,y,z,range and uint8 status",name.c_str());return;}
                    const float nan=std::numeric_limits<float>::quiet_NaN();
                    for(size_t base=0;base+cloud.point_step<=cloud.data.size();base+=cloud.point_step) {
                        auto* point=cloud.data.data()+base;
                        if(point[fields->status]!=0) continue; // Only real returns get range noise or drops.
                        float xyzr[4];
                        std::memcpy(&xyzr[0],point+fields->x,4);std::memcpy(&xyzr[1],point+fields->y,4);
                        std::memcpy(&xyzr[2],point+fields->z,4);std::memcpy(&xyzr[3],point+fields->range,4);
                        const auto range=model->range(xyzr[3]);
                        if(!range) {point[fields->status]=2;xyzr[0]=xyzr[1]=xyzr[2]=xyzr[3]=nan;}
                        else {const float scale=xyzr[3]>0?static_cast<float>(*range/xyzr[3]):0.f;for(int i=0;i<3;++i) xyzr[i]*=scale;xyzr[3]=static_cast<float>(*range);}
                        std::memcpy(point+fields->x,&xyzr[0],4);std::memcpy(point+fields->y,&xyzr[1],4);
                        std::memcpy(point+fields->z,&xyzr[2],4);std::memcpy(point+fields->range,&xyzr[3],4);
                    }
                }));
        }
        reconcile_=create_wall_timer(200ms,[this]{for_each([](auto& c){c.reconcile();});});
        release_=rclcpp::create_timer(this,get_clock(),2ms,[this]{const double now=this->now().seconds();for_each([now](auto& c){c.release(now);});});
    }
private:
    template<class F> void for_each(F&& f) {f(*imu_);f(*gnss_);f(*wheels_);for(auto& l:lidars_) f(*l);}
    std::unique_ptr<degradation::ImuModel> imu_model_;
    std::unique_ptr<degradation::GnssModel> gnss_model_;
    std::unique_ptr<degradation::WheelModel> wheel_model_;
    std::unique_ptr<Channel<sensor_msgs::msg::Imu>> imu_;
    std::unique_ptr<Channel<sensor_msgs::msg::NavSatFix>> gnss_;
    std::unique_ptr<Channel<ets2_msgs::msg::WheelState>> wheels_;
    std::vector<std::unique_ptr<Channel<sensor_msgs::msg::PointCloud2>>> lidars_;
    rclcpp::TimerBase::SharedPtr reconcile_,release_;
};

int main(int argc,char** argv) {
    rclcpp::init(argc,argv);
    rclcpp::spin(std::make_shared<SensorDegradation>());
    rclcpp::shutdown();
}
