#include "clamp.hpp"
#include "cv_to_pv.hpp"
#include "format_mapping.hpp"
#include "parameter_conflict_check.hpp"
#include "pretty_print.hpp"
#include "pv_to_cv.hpp"
#include "type_extent.hpp"
#include "types.hpp"
#include <algorithm>
#include <array>
#include <camera_info_manager/camera_info_manager.hpp>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#if __has_include(<cv_bridge/cv_bridge.hpp>)
#include <cv_bridge/cv_bridge.hpp>
#elif __has_include(<cv_bridge/cv_bridge.h>)
#include <cv_bridge/cv_bridge.h>
#endif
#include <functional>
#include <iostream>
#include <libcamera/base/shared_fd.h>
#include <libcamera/base/signal.h>
#include <libcamera/base/span.h>
#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>
#include <libcamera/controls.h>
#include <libcamera/framebuffer.h>
#include <libcamera/framebuffer_allocator.h>
#include <libcamera/geometry.h>
#include <libcamera/pixel_format.h>
#include <libcamera/property_ids.h>
#include <libcamera/request.h>
#include <libcamera/stream.h>
#include <map>
#include <memory>
#include <mutex>
#include <opencv2/imgcodecs.hpp>
#include <optional>
#include <rcl/context.h>
#include <rcl_interfaces/msg/detail/floating_point_range__struct.hpp>
#include <rcl_interfaces/msg/detail/integer_range__struct.hpp>
#include <rcl_interfaces/msg/detail/parameter_descriptor__struct.hpp>
#include <rcl_interfaces/msg/detail/set_parameters_result__struct.hpp>
#include <rclcpp/logging.hpp>
#include <rclcpp/node.hpp>
#include <rclcpp/node_interfaces/node_parameters_interface.hpp>
#include <rclcpp/parameter.hpp>
#include <rclcpp/parameter_value.hpp>
#include <rclcpp/publisher.hpp>
#include <rclcpp/time.hpp>
#include <rclcpp_components/register_node_macro.hpp>
#include <sensor_msgs/image_encodings.hpp>
#include <sensor_msgs/msg/detail/camera_info__struct.hpp>
#include <sensor_msgs/msg/detail/compressed_image__struct.hpp>
#include <sensor_msgs/msg/detail/image__struct.hpp>
#include <std_msgs/msg/detail/header__struct.hpp>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
namespace enc = sensor_msgs::image_encodings;
namespace rclcpp
{
class NodeOptions;
}


namespace camera
{
class CameraNode : public rclcpp::Node
{
public:
  explicit CameraNode(const rclcpp::NodeOptions &options);

  ~CameraNode();

private:
  libcamera::CameraManager camera_manager;
  std::shared_ptr<libcamera::Camera> camera;
  libcamera::Stream *stream;
  std::shared_ptr<libcamera::FrameBufferAllocator> allocator;
  std::vector<std::unique_ptr<libcamera::Request>> requests;
  std::vector<std::thread> request_threads;
  std::unordered_map<libcamera::Request *, std::unique_ptr<std::mutex>> request_locks;
  std::atomic<bool> running;

  struct buffer_info_t
  {
    void *data;
    size_t size;
  };
  std::unordered_map<const libcamera::FrameBuffer *, buffer_info_t> buffer_info;

  // timestamp offset (ns) from camera time to system time
  int64_t time_offset = 0;

  rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr pub_image;
  rclcpp::Publisher<sensor_msgs::msg::CompressedImage>::SharedPtr pub_image_compressed;
  rclcpp::Publisher<sensor_msgs::msg::CameraInfo>::SharedPtr pub_ci;

  camera_info_manager::CameraInfoManager cim;

  OnSetParametersCallbackHandle::SharedPtr callback_parameter_change;

  // map parameter names to libcamera control id
  std::unordered_map<std::string, const libcamera::ControlId *> parameter_ids;
  // parameters that are to be set for every request
  std::unordered_map<unsigned int, libcamera::ControlValue> parameters;
  // keep track of set parameters
  ParameterMap parameters_full;
  std::mutex parameters_lock;
  // compression quality parameter
  std::atomic_uint8_t jpeg_quality;

  void
  declareParameters();

  void
  requestComplete(libcamera::Request *const request);

  void
  process(libcamera::Request *const request);

  rcl_interfaces::msg::SetParametersResult
  onParameterChange(const std::vector<rclcpp::Parameter> &parameters);
};

RCLCPP_COMPONENTS_REGISTER_NODE(camera::CameraNode)


libcamera::StreamRole
get_role(const std::string &role)
{
  static const std::unordered_map<std::string, libcamera::StreamRole> roles_map = {
    {"raw", libcamera::StreamRole::Raw},
    {"still", libcamera::StreamRole::StillCapture},
    {"video", libcamera::StreamRole::VideoRecording},
    {"viewfinder", libcamera::StreamRole::Viewfinder},
  };

  try {
    return roles_map.at(role);
  }
  catch (const std::out_of_range &) {
    throw std::runtime_error("invalid stream role: \"" + role + "\"");
  }
}

CameraNode::CameraNode(const rclcpp::NodeOptions &options) : Node("camera", options), cim(this)
{
  // pixel format
  rcl_interfaces::msg::ParameterDescriptor param_descr_format;
  param_descr_format.description = "pixel format of streaming buffer";
  param_descr_format.read_only = true;
  declare_parameter<std::string>("format", {}, param_descr_format);

  // stream role
  rcl_interfaces::msg::ParameterDescriptor param_descr_role;
  param_descr_role.description = "stream role";
  param_descr_role.additional_constraints = "one of {raw, still, video, viewfinder}";
  param_descr_role.read_only = true;
  declare_parameter<std::string>("role", "video", param_descr_role);

  // image dimensions
  rcl_interfaces::msg::ParameterDescriptor param_descr_ro;
  param_descr_ro.read_only = true;
  declare_parameter<int64_t>("width", {}, param_descr_ro);
  declare_parameter<int64_t>("height", {}, param_descr_ro);

  // camera ID
  declare_parameter("camera", rclcpp::ParameterValue {}, param_descr_ro.set__dynamic_typing(true));

  rcl_interfaces::msg::ParameterDescriptor jpeg_quality_description;
  jpeg_quality_description.name = "jpeg_quality";
  jpeg_quality_description.type = rcl_interfaces::msg::ParameterType::PARAMETER_INTEGER;
  jpeg_quality_description.description = "Image quality for JPEG format";
  jpeg_quality_description.read_only = false;
  rcl_interfaces::msg::IntegerRange jpeg_range;
  jpeg_range.from_value = 1;
  jpeg_range.to_value = 100;
  jpeg_range.step = 1;
  jpeg_quality_description.integer_range = {jpeg_range};
  // default to 95
  jpeg_quality = declare_parameter<uint8_t>("jpeg_quality", 95, jpeg_quality_description);
  // publisher for raw and compressed image
  pub_image = this->create_publisher<sensor_msgs::msg::Image>("~/image_raw", 1);
  pub_image_compressed =
    this->create_publisher<sensor_msgs::msg::CompressedImage>("~/image_raw/compressed", 1);
  pub_ci = this->create_publisher<sensor_msgs::msg::CameraInfo>("~/camera_info", 1);

  // start camera manager and check for cameras
  camera_manager.start();
  if (camera_manager.cameras().empty())
    throw std::runtime_error("no cameras available");

  // get the camera
  switch (get_parameter("camera").get_type()) {
  case rclcpp::ParameterType::PARAMETER_NOT_SET:
    // use first camera as default
    camera = camera_manager.cameras().front();
    RCLCPP_INFO_STREAM(get_logger(), camera_manager);
    RCLCPP_WARN_STREAM(get_logger(),
                       "no camera selected, using default: \"" << camera->id() << "\"");
    RCLCPP_WARN_STREAM(get_logger(), "set parameter 'camera' to silent this warning");
    break;
  case rclcpp::ParameterType::PARAMETER_INTEGER:
  {
    const size_t id = get_parameter("camera").as_int();
    if (id >= camera_manager.cameras().size()) {
      RCLCPP_INFO_STREAM(get_logger(), camera_manager);
      throw std::runtime_error("camera with id " + std::to_string(id) + " does not exist");
    }
    camera = camera_manager.cameras().at(id);
    RCLCPP_DEBUG_STREAM(get_logger(), "found camera by id: " << id);
  } break;
  case rclcpp::ParameterType::PARAMETER_STRING:
  {
    const std::string name = get_parameter("camera").as_string();
    camera = camera_manager.get(name);
    if (!camera) {
      RCLCPP_INFO_STREAM(get_logger(), camera_manager);
      throw std::runtime_error("camera with name " + name + " does not exist");
    }
    RCLCPP_DEBUG_STREAM(get_logger(), "found camera by name: \"" << name << "\"");
  } break;
  default:
    RCLCPP_ERROR_STREAM(get_logger(), "unuspported camera parameter type: "
                                        << get_parameter("camera").get_type_name());
    break;
  }

  if (!camera)
    throw std::runtime_error("failed to find camera");

  if (camera->acquire())
    throw std::runtime_error("failed to acquire camera");

  // configure camera stream
  std::unique_ptr<libcamera::CameraConfiguration> cfg =
    camera->generateConfiguration({get_role(get_parameter("role").as_string())});

  if (!cfg)
    throw std::runtime_error("failed to generate configuration");

  libcamera::StreamConfiguration &scfg = cfg->at(0);
  // get common pixel formats that are supported by the camera and the node
  const libcamera::StreamFormats stream_formats = get_common_stream_formats(scfg.formats());
  const std::vector<libcamera::PixelFormat> common_fmt = stream_formats.pixelformats();

  // list all camera formats, including those not supported by the ROS message
  RCLCPP_DEBUG_STREAM(get_logger(), scfg.formats());

  if (common_fmt.empty())
    throw std::runtime_error("camera does not provide any of the supported pixel formats");

  const std::string format = get_parameter("format").as_string();
  if (format.empty()) {
    // check that the default pixel format is supported by the ROS encoding
    if (std::find(common_fmt.cbegin(), common_fmt.cend(), scfg.pixelFormat) == common_fmt.cend()) {
      // auto select first common pixel format
      scfg.pixelFormat = common_fmt.front();
    }

    RCLCPP_INFO_STREAM(get_logger(), stream_formats);
    RCLCPP_WARN_STREAM(get_logger(),
                       "no pixel format selected, using default: \"" << scfg.pixelFormat << "\"");
    RCLCPP_WARN_STREAM(get_logger(), "set parameter 'format' to silent this warning");
  }
  else {
    // get pixel format from provided string
    const libcamera::PixelFormat format_requested = libcamera::PixelFormat::fromString(format);
    if (!format_requested.isValid()) {
      RCLCPP_INFO_STREAM(get_logger(), stream_formats);
      throw std::runtime_error("invalid pixel format: \"" + format + "\"");
    }
    // check that the requested format is supported by camera and the node
    if (std::find(common_fmt.begin(), common_fmt.end(), format_requested) == common_fmt.end()) {
      RCLCPP_INFO_STREAM(get_logger(), stream_formats);
      throw std::runtime_error("unsupported pixel format \"" + format + "\"");
    }
    scfg.pixelFormat = format_requested;
  }

  const libcamera::Size size(get_parameter("width").as_int(), get_parameter("height").as_int());
  if (size.isNull()) {
    RCLCPP_INFO_STREAM(get_logger(), scfg);
    RCLCPP_WARN_STREAM(get_logger(),
                       "no dimensions selected, auto-selecting: \"" << scfg.size << "\"");
    RCLCPP_WARN_STREAM(get_logger(), "set parameter 'width' or 'height' to silent this warning");
  }
  else {
    scfg.size = size;
  }

  // store selected stream configuration
  const libcamera::StreamConfiguration selected_scfg = scfg;

  switch (cfg->validate()) {
  case libcamera::CameraConfiguration::Valid:
    break;
  case libcamera::CameraConfiguration::Adjusted:
    if (selected_scfg.pixelFormat != scfg.pixelFormat)
      RCLCPP_INFO_STREAM(get_logger(), stream_formats);
    if (selected_scfg.size != scfg.size)
      RCLCPP_INFO_STREAM(get_logger(), scfg);
    RCLCPP_WARN_STREAM(get_logger(), "stream configuration adjusted from \""
                                       << selected_scfg.toString() << "\" to \"" << scfg.toString()
                                       << "\"");
    break;
  case libcamera::CameraConfiguration::Invalid:
    throw std::runtime_error("failed to valid stream configurations");
    break;
  }

  if (camera->configure(cfg.get()) < 0)
    throw std::runtime_error("failed to configure streams");

  RCLCPP_INFO_STREAM(get_logger(), "camera \"" << camera->id() << "\" configured with "
                                               << scfg.toString() << " stream");

  set_parameter(rclcpp::Parameter("width", int64_t(scfg.size.width)));
  set_parameter(rclcpp::Parameter("height", int64_t(scfg.size.height)));
  set_parameter(rclcpp::Parameter("format", scfg.pixelFormat.toString()));

  // format camera name for calibration file
  const libcamera::ControlList &props = camera->properties();
  std::string cname = camera->id() + '_' + scfg.size.toString();
  const std::optional<std::string> model = props.get(libcamera::properties::Model);
  if (model)
    cname = model.value() + '_' + cname;

  // clean camera name of non-alphanumeric characters
  cname.erase(
    std::remove_if(cname.begin(), cname.end(), [](const char &x) { return std::isspace(x); }),
    cname.cend());
  std::replace_if(
    cname.begin(), cname.end(), [](const char &x) { return !std::isalnum(x); }, '_');

  if (!cim.setCameraName(cname))
    throw std::runtime_error("camera name must only contain alphanumeric characters");

  declareParameters();

  // allocate stream buffers and create one request per buffer
  stream = scfg.stream();

  allocator = std::make_shared<libcamera::FrameBufferAllocator>(camera);
  allocator->allocate(stream);

  for (const std::unique_ptr<libcamera::FrameBuffer> &buffer : allocator->buffers(stream)) {
    std::unique_ptr<libcamera::Request> request = camera->createRequest();
    if (!request)
      throw std::runtime_error("Can't create request");

    // multiple planes of the same buffer use the same file descriptor
    size_t buffer_length = 0;
    int fd = -1;
    for (const libcamera::FrameBuffer::Plane &plane : buffer->planes()) {
      if (plane.offset == libcamera::FrameBuffer::Plane::kInvalidOffset)
        throw std::runtime_error("invalid offset");
      buffer_length = std::max<size_t>(buffer_length, plane.offset + plane.length);
      if (!plane.fd.isValid())
        throw std::runtime_error("file descriptor is not valid");
      if (fd == -1)
        fd = plane.fd.get();
      else if (fd != plane.fd.get())
        throw std::runtime_error("plane file descriptors differ");
    }

    // memory-map the frame buffer planes
    void *data = mmap(nullptr, buffer_length, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED)
      throw std::runtime_error("mmap failed: " + std::string(std::strerror(errno)));
    buffer_info[buffer.get()] = {data, buffer_length};

    if (request->addBuffer(stream, buffer.get()) < 0)
      throw std::runtime_error("Can't set buffer for request");

    requests.push_back(std::move(request));
  }

  // create a processing thread per request
  for (const std::unique_ptr<libcamera::Request> &request : requests) {
    request_locks[request.get()] = std::make_unique<std::mutex>();
    request_locks[request.get()]->lock();
    running = true;
    request_threads.emplace_back(&CameraNode::process, this, request.get());
  }

  // register callback
  camera->requestCompleted.connect(this, &CameraNode::requestComplete);

  libcamera::ControlList controls_ = camera->controls();
  for (const auto &[id, value] : parameters)
    controls_.set(id, value);

  // start camera and queue all requests
  if (camera->start(&controls_))
    throw std::runtime_error("failed to start camera");

  for (std::unique_ptr<libcamera::Request> &request : requests)
    camera->queueRequest(request.get());
}

CameraNode::~CameraNode()
{
  // stop request processing threads
  running = false;
  for (std::thread &thread : request_threads)
    thread.join();

  // stop camera
  if (camera->stop())
    std::cerr << "failed to stop camera" << std::endl;
  allocator->free(stream);
  allocator.reset();
  camera->release();
  camera.reset();
  camera_manager.stop();
  for (const auto &e : buffer_info)
    if (munmap(e.second.data, e.second.size) == -1)
      std::cerr << "munmap failed: " << std::strerror(errno) << std::endl;
}

void
CameraNode::declareParameters()
{
  // dynamic camera configuration
  ParameterMap parameters_init;
  for (const auto &[id, info] : camera->controls()) {
    // store control id with name
    parameter_ids[id->name()] = id;

    std::size_t extent;
    try {
      extent = get_extent(id);
    }
    catch (const std::runtime_error &e) {
      // ignore
      RCLCPP_WARN_STREAM(get_logger(), e.what());
      continue;
    }

    // format type description
    const std::string cv_descr =
      std::to_string(id->type()) + " " +
      std::string(extent > 1 ? "array[" + std::to_string(extent) + "]" : "scalar") + " range {" +
      info.min().toString() + "}..{" + info.max().toString() + "}" +
      (info.def().isNone() ? std::string {} : " (default: {" + info.def().toString() + "})");

    if (info.min().numElements() != info.max().numElements())
      throw std::runtime_error("minimum and maximum parameter array sizes do not match");

    // clamp default ControlValue to min/max range and cast ParameterValue
    const rclcpp::ParameterValue value = cv_to_pv(clamp(info.def(), info.min(), info.max()), extent);

    // get smallest bounds for minimum and maximum set
    rcl_interfaces::msg::IntegerRange range_int;
    rcl_interfaces::msg::FloatingPointRange range_float;

    switch (id->type()) {
    case libcamera::ControlTypeInteger32:
      range_int.from_value = max<libcamera::ControlTypeInteger32>(info.min());
      range_int.to_value = min<libcamera::ControlTypeInteger32>(info.max());
      break;
    case libcamera::ControlTypeInteger64:
      range_int.from_value = max<libcamera::ControlTypeInteger64>(info.min());
      range_int.to_value = min<libcamera::ControlTypeInteger64>(info.max());
      break;
    case libcamera::ControlTypeFloat:
      range_float.from_value = max<libcamera::ControlTypeFloat>(info.min());
      range_float.to_value = min<libcamera::ControlTypeFloat>(info.max());
      break;
    default:
      break;
    }

    rcl_interfaces::msg::ParameterDescriptor param_descr;
    param_descr.description = cv_descr;
    if (range_int.from_value != range_int.to_value)
      param_descr.integer_range = {range_int};
    if (range_float.from_value != range_float.to_value)
      param_descr.floating_point_range = {range_float};

    // declare parameters and set default or initial value
    RCLCPP_DEBUG_STREAM(get_logger(),
                        "declare " << id->name() << " with default " << rclcpp::to_string(value));
    if (value.get_type() == rclcpp::ParameterType::PARAMETER_NOT_SET) {
      declare_parameter(id->name(), cv_to_pv_type(id->type(), extent > 0), param_descr);
    }
    else {
      declare_parameter(id->name(), value, param_descr);
      parameters_init[id->name()] = value;
    }
  }

  // register callback to handle parameter changes
  // We have to register the callback after parameter declaration
  // to avoid callbacks interfering with the default parameter check.
  callback_parameter_change = add_on_set_parameters_callback(
    std::bind(&CameraNode::onParameterChange, this, std::placeholders::_1));

  // resolve conflicts of default libcamera configuration and user provided overrides
  std::vector<std::string> status;
  std::tie(parameters_init, status) =
    resolve_conflicts(parameters_init, get_node_parameters_interface()->get_parameter_overrides());

  for (const std::string &s : status)
    RCLCPP_WARN_STREAM(get_logger(), s);

  std::vector<rclcpp::Parameter> parameters_init_list;
  for (const auto &[name, value] : parameters_init)
    parameters_init_list.emplace_back(name, value);
  set_parameters(parameters_init_list);
}

// The following function "compressImageMsg" is adapted from "CvImage::toCompressedImageMsg"
// (https://github.com/ros-perception/vision_opencv/blob/066793a23e5d06d76c78ca3d69824a501c3554fd/cv_bridge/src/cv_bridge.cpp#L512-L535)
// and covered by the BSD-3-Clause licence.
void
compressImageMsg(const sensor_msgs::msg::Image &source,
                 sensor_msgs::msg::CompressedImage &destination,
                 const std::vector<int> &params = std::vector<int>())
{
  std::shared_ptr<CameraNode> tracked_object;
  cv_bridge::CvImageConstPtr cv_ptr = cv_bridge::toCvShare(source, tracked_object);

  destination.header = source.header;
  cv::Mat image;
  if (cv_ptr->encoding == enc::BGR8 || cv_ptr->encoding == enc::BGRA8 ||
      cv_ptr->encoding == enc::MONO8 || cv_ptr->encoding == enc::MONO16)
  {
    image = cv_ptr->image;
  }
  else {
    cv_bridge::CvImagePtr tempThis = std::make_shared<cv_bridge::CvImage>(*cv_ptr);
    cv_bridge::CvImagePtr temp;
    if (enc::hasAlpha(cv_ptr->encoding)) {
      temp = cv_bridge::cvtColor(tempThis, enc::BGRA8);
    }
    else {
      temp = cv_bridge::cvtColor(tempThis, enc::BGR8);
    }
    image = temp->image;
  }

  destination.format = "jpg";
  cv::imencode(".jpg", image, destination.data, params);
}

void
CameraNode::requestComplete(libcamera::Request *const request)
{
  request_locks[request]->unlock();
}

void
CameraNode::process(libcamera::Request *const request)
{
  while (running) {
    // block until request is available
    request_locks[request]->lock();

    if (request->status() == libcamera::Request::RequestComplete) {
      assert(request->buffers().size() == 1);

      // get the stream and buffer from the request
      const libcamera::FrameBuffer *buffer = request->findBuffer(stream);
      const libcamera::FrameMetadata &metadata = buffer->metadata();
      size_t bytesused = 0;
      for (const libcamera::FrameMetadata::Plane &plane : metadata.planes())
        bytesused += plane.bytesused;

      // set time offset once for accurate timing using the device time
      if (time_offset == 0)
        time_offset = this->now().nanoseconds() - metadata.timestamp;

      // send image data
      std_msgs::msg::Header hdr;
      hdr.stamp = rclcpp::Time(time_offset + int64_t(metadata.timestamp));
      hdr.frame_id = "camera";
      const libcamera::StreamConfiguration &cfg = stream->configuration();

      auto msg_img = std::make_unique<sensor_msgs::msg::Image>();
      auto msg_img_compressed = std::make_unique<sensor_msgs::msg::CompressedImage>();

      if (format_type(cfg.pixelFormat) == FormatType::RAW) {
        // raw uncompressed image
        assert(buffer_info[buffer].size == bytesused);
        msg_img->header = hdr;
        msg_img->width = cfg.size.width;
        msg_img->height = cfg.size.height;
        msg_img->step = cfg.stride;
        msg_img->encoding = get_ros_encoding(cfg.pixelFormat);
        msg_img->is_bigendian = (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__);
        msg_img->data.resize(buffer_info[buffer].size);
        memcpy(msg_img->data.data(), buffer_info[buffer].data, buffer_info[buffer].size);

        // compress to jpeg
        if (pub_image_compressed->get_subscription_count()) {
          try {
            compressImageMsg(*msg_img, *msg_img_compressed,
                             {cv::IMWRITE_JPEG_QUALITY, jpeg_quality});
          }
          catch (const cv_bridge::Exception &e) {
            RCLCPP_ERROR_STREAM(get_logger(), e.what());
          }
        }
      }
      else if (format_type(cfg.pixelFormat) == FormatType::COMPRESSED) {
        // compressed image
        assert(bytesused < buffer_info[buffer].size);
        msg_img_compressed->header = hdr;
        msg_img_compressed->format = get_ros_encoding(cfg.pixelFormat);
        msg_img_compressed->data.resize(bytesused);
        memcpy(msg_img_compressed->data.data(), buffer_info[buffer].data, bytesused);

        // decompress into raw rgb8 image
        if (pub_image->get_subscription_count())
          cv_bridge::toCvCopy(*msg_img_compressed, "rgb8")->toImageMsg(*msg_img);
      }
      else {
        throw std::runtime_error("unsupported pixel format: " +
                                 stream->configuration().pixelFormat.toString());
      }

      pub_image->publish(std::move(msg_img));
      pub_image_compressed->publish(std::move(msg_img_compressed));

      sensor_msgs::msg::CameraInfo ci = cim.getCameraInfo();
      ci.header = hdr;
      pub_ci->publish(ci);
    }
    else if (request->status() == libcamera::Request::RequestCancelled) {
      RCLCPP_ERROR_STREAM(get_logger(), "request '" << request->toString() << "' cancelled");
    }

    // queue the request again for the next frame
    request->reuse(libcamera::Request::ReuseBuffers);

    // update parameters
    parameters_lock.lock();
    for (const auto &[id, value] : parameters)
      request->controls().set(id, value);
    parameters.clear();
    parameters_lock.unlock();

    camera->queueRequest(request);
  }
}

rcl_interfaces::msg::SetParametersResult
CameraNode::onParameterChange(const std::vector<rclcpp::Parameter> &parameters)
{
  rcl_interfaces::msg::SetParametersResult result;

  // check target parameter state (current and new parameters)
  // for conflicting configuration
  const std::vector<std::string> msgs = check_conflicts(parameters, parameters_full);
  if (!msgs.empty()) {
    result.successful = false;
    for (size_t i = 0; i < msgs.size(); i++) {
      if (msgs.size() > 1)
        result.reason += "(" + std::to_string(i) + ") ";
      result.reason += msgs[i];
      if (i < msgs.size() - 1)
        result.reason += "; ";
    }
    return result;
  }

  result.successful = true;

  for (const rclcpp::Parameter &parameter : parameters) {
    RCLCPP_DEBUG_STREAM(get_logger(), "setting " << parameter.get_type_name() << " parameter "
                                                 << parameter.get_name() << " to "
                                                 << parameter.value_to_string());

    if (parameter_ids.count(parameter.get_name())) {
      const libcamera::ControlId *id = parameter_ids.at(parameter.get_name());
      libcamera::ControlValue value = pv_to_cv(parameter, id->type());

      if (!value.isNone()) {
        // verify parameter type and dimension against default
        const libcamera::ControlInfo &ci = camera->controls().at(id);

        if (value.type() != id->type()) {
          result.successful = false;
          result.reason = parameter.get_name() + ": parameter types mismatch, expected '" +
                          std::to_string(id->type()) + "', got '" + std::to_string(value.type()) +
                          "'";
          return result;
        }

        const std::size_t extent = get_extent(id);
        if ((value.isArray() && (extent > 0)) && value.numElements() != extent) {
          result.successful = false;
          result.reason = parameter.get_name() + ": parameter dimensions mismatch, expected " +
                          std::to_string(extent) + ", got " + std::to_string(value.numElements());
          return result;
        }

        // check bounds and return error
        if (value < ci.min() || value > ci.max()) {
          result.successful = false;
          result.reason =
            "parameter value " + value.toString() + " outside of range: " + ci.toString();
          return result;
        }

        parameters_lock.lock();
        this->parameters[parameter_ids.at(parameter.get_name())->id()] = value;
        parameters_lock.unlock();

        parameters_full[parameter.get_name()] = parameter.get_parameter_value();
      }
    }
    else if (!parameter.get_name().compare("jpeg_quality")) {
      jpeg_quality = parameter.get_parameter_value().get<uint8_t>();
    }
  }

  return result;
}

} // namespace camera
