#include "ffmpegdecoder.h"

#include <cmath>

#include "ffmpeghelper.h"
#include "common/singleton.h"
#include "config/config.h"
#include "spdlog/spdlog.h"
#include "util/cthread.h"
#include "widget/video_display/video_display_widget.h"

void Decoder::Init(AVCodecContext* codec_ctx, std::shared_ptr<PacketQueue> pkt_queue,
                   std::shared_ptr<FrameQueue> frame_queue)
{
    codec_ctx_ = codec_ctx;
    pkt_queue_ = pkt_queue;
    frame_queue_ = frame_queue;
    thread_ = std::make_unique<std::thread>(&Decoder::Decode, this);
}

void Decoder::Decode()
{
    auto frame = av_frame_alloc();

    Frame* frame_writable = nullptr;
    for (;;) {
        if (!(frame_writable = frame_queue_->PeekWritable())) {
            return;
        }

        AVPacket pkt;
        pkt_queue_->Pop(&pkt);

        DecodeOneFrame(pkt, frame);

        av_frame_move_ref(frame_writable->frame_, frame);
        frame_writable->serial_ = 0;

        frame_queue_->Push();
    }
}

void VideoDecoder::DecodeOneFrame(const AVPacket& pkt, AVFrame* frame)
{
    int ret = avcodec_send_packet(codec_ctx_, &pkt);
    if (ret != 0) {
        SPDLOG_WARN("Failed to send packet to codec.");
    }

    avcodec_receive_frame(codec_ctx_, frame);
}

void FrameQueue::Push()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_++;
        write_index_ = ++write_index_ % max_size_;
    }
}

FrameQueue::FrameQueue(int max_size)
    : serial_(-1)
    , max_size_(max_size)
    , size_(0)
    , write_index_(0)
{}

bool FrameQueue::InitFrameQueue()
{
    memset(frame_queue_, 0, sizeof(Frame) * FRAME_QUEUE_SIZE);
    for (int i = 0; i < max_size_; i++) {
        frame_queue_[i].serial_ = -1;
        frame_queue_[i].frame_ = av_frame_alloc();
        if (!frame_queue_[i].frame_) {
            SPDLOG_ERROR("Failed to alloc queue frame, index: {0}", i);
            return false;
        }
    }

    return true;
}

Frame* FrameQueue::PeekWritable()
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (size_ >= max_size_) {
            cv_.wait(lock);
        }
    }

    if (false) {
        return nullptr;
    }

    return &frame_queue_[write_index_];
}


PacketQueue::PacketQueue()
    : serial_(0)
{
    pkt_list_ = av_fifo_alloc2(1, sizeof(AVPacketInfo), AV_FIFO_FLAG_AUTO_GROW);
}

void PacketQueue::Push(AVPacket* pkt)
{
    AVPacketInfo pkt_info;
    pkt_info.serial_ = 0;
    pkt_info.pkt_ = av_packet_alloc();
    if (!pkt_info.pkt_) {
        SPDLOG_ERROR("Failed to alloc packet, no memory");
        return;
    }

    av_packet_move_ref(pkt_info.pkt_, pkt);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        av_fifo_write(pkt_list_, &pkt_info, 1);
    }
}

void PacketQueue::Pop(AVPacket* pkt)
{
    AVPacketInfo pkt_info;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        av_fifo_read(pkt_list_, &pkt_info, 1);
    }

    av_packet_move_ref(pkt, pkt_info.pkt_);
}

FFmpegProcessor::FFmpegProcessor()
    : eof_(true)
    , fmt_ctx_(nullptr)
    , codec_ctx_(nullptr)
    , sws_ctx_(nullptr)
    , video_stream_(nullptr)
    , packet_(nullptr)
    , frame_(nullptr)
    , hw_decode_(false)
    , hw_frame_(nullptr)
    , hw_dev_ctx_(nullptr)
    , block_start_time_(0)
    , block_timeout_(10)
    , fps_(0)
{
    video_queue_ = std::make_shared<PacketQueue>();

    memset(data_, 0, sizeof(uint8_t*) * 4);
    memset(linesize_, 0, sizeof(int) * 4);

    // Find hardware codec devices.
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
        hw_devices_.push_back(type);
    }

    // Register input device.
    avdevice_register_all();
}

FFmpegProcessor::~FFmpegProcessor()
{
    Close();
}

static int OnInterrupt(void* opaque)
{
    const FFmpegProcessor* decoder = static_cast<FFmpegProcessor*>(opaque);
    if (!decoder)
        return 0;

    if (time(nullptr) - decoder->block_start_time() > decoder->block_timeout()) {
        SPDLOG_ERROR("A timeout occurred for an I/O-related operation, media: {0}.",
                     decoder->media().src);
        return 1;
    }

    return 0;
}

bool FFmpegProcessor::Open()
{
    // Auto release resource when abnormal conditions occur.
    DEFER(if (eof_) { Close(); })

    if (!OpenInputFormat()) {
        return false;
    }

    if (!AllocFrame()) {
        return false;
    }

    DoScalePrepare();

    // read
    for (;;) {
        int ret = av_read_frame(fmt_ctx_, packet_);
        if (ret == 0) {
            video_queue_->Push(packet_);
        } else if (ret == AVERROR_EOF) {
            if (stream_indexs_[AVMEDIA_TYPE_VIDEO] >= 0) {
                PushNullPacket(video_queue_, packet_, stream_indexs_[AVMEDIA_TYPE_VIDEO]);
            }
            /*  if (stream_indexs_[AVMEDIA_TYPE_AUDIO] >= 0) {
                  PushNullPacket(video_queue_, packet_, stream_indexs_[]);
              }

              if (stream_indexs_[AVMEDIA_TYPE_SUBTITLE] >= 0) {
                  PushNullPacket(video_queue_, packet_, stream_indexs_[]);
              }*/
            break;
        } else {
            FFmpegHelper::FFmpegError(ret);
            continue;
        }
    }

    eof_ = false;
    return true;
}

int FFmpegProcessor::GetPacket(AVPacket* pkt)
{
    int ret;

    do {
        av_packet_unref(pkt);

        block_start_time_ = time(nullptr);
        ret = av_read_frame(fmt_ctx_, pkt);
    } while (pkt->stream_index != video_stream_->index && ret >= 0);

    return ret;
}

static int GetCommonFmt(int format)
{
    switch (format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        return PIX_FMT_IYUV;
    case AV_PIX_FMT_NV12:
        return PIX_FMT_NV12;
    default:
        return 0;
    }
}

DecodeFrame* FFmpegProcessor::GetFrame()
{
    int ret = GetPacket(packet_);
    if (ret == 0) {
        if (packet_->stream_index == video_stream_->index) {
            if (avcodec_send_packet(codec_ctx_, packet_) != 0) {
                FFmpegHelper::FFmpegError(ret);
            }
        }
    } else if (ret == AVERROR_EOF) {
        avcodec_send_packet(codec_ctx_, nullptr); // Flush decoder
    } else {
        FFmpegHelper::FFmpegError(ret);
    }
    av_packet_unref(packet_);

    int error_code = avcodec_receive_frame(codec_ctx_, frame_);
    if (error_code == AVERROR(EAGAIN) || error_code == AVERROR_EOF) {
        av_frame_unref(frame_);

        if (ret < 0) {
            eof_ = true;
        }
        return nullptr;
    }

    AVFrame* frame = frame_;
    if (hw_decode_) {
        frame = hw_frame_;

        ret = GpuDataToCpu(frame_, hw_frame_);
        av_frame_unref(frame_);

        if (!ret) {
            return nullptr;
        }
    }

    decode_frame_.pict_type_ = frame->pict_type;
    ret = Scale(frame);
    av_frame_unref(frame);

    if (!ret) {
        return nullptr;
    }

    return &decode_frame_;
}

bool FFmpegProcessor::AllocFrame()
{
    packet_ = av_packet_alloc();
    if (!packet_) {
        SPDLOG_ERROR("Failed to alloc packet.");
        return false;
    }
    frame_ = av_frame_alloc();
    if (!frame_) {
        SPDLOG_ERROR("Failed to alloc frame.");
        return false;
    }

    hw_frame_ = av_frame_alloc();
    if (!hw_frame_) {
        SPDLOG_ERROR("Failed to alloc hw frame.");
        return false;
    }

    return true;
}

void FFmpegProcessor::DoScalePrepare()
{
    int dst_w;
    int dst_h;
    AlignSize(video_stream_->codecpar->width, video_stream_->codecpar->height, &dst_w, &dst_h);

    // Convert to uniform format.
    AVPixelFormat dst_pix_fmt = GetDstPixFormat();
    ResizeDecodeFrame(dst_w, dst_h, dst_pix_fmt);

    encode_info_.w = dst_w;
    encode_info_.h = dst_h;
    encode_info_.fps = fps_;
    encode_info_.compression = compression_type(video_stream_->codecpar->codec_id);
}

static EncodeFormat compression_type(AVCodecID codec_id)
{
    switch (codec_id) {
    case AV_CODEC_ID_H264:
        return H264;
    case AV_CODEC_ID_HEVC:
        return HEVC;
    case AV_CODEC_ID_MJPEG:
        return MJPEG;
    default:
        return H264;
    }
}

void FFmpegProcessor::Close()
{
    if (fmt_ctx_) {
        avformat_close_input(&fmt_ctx_);
    }

    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
    }

    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    if (packet_) {
        av_packet_free(&packet_);
    }

    if (frame_) {
        av_frame_free(&frame_);
    }

    if (hw_frame_) {
        av_frame_free(&hw_frame_);
    }

    if (hw_dev_ctx_) {
        av_buffer_unref(&hw_dev_ctx_);
    }

    decode_frame_.buf.cleanup(); // Free memory
}

bool FFmpegProcessor::OpenInputFormat()
{
    std::string url;
    AVInputFormat* input_fmt = nullptr;
    if (!InputFmt(url, &input_fmt))
        return false;

    // Demuxer
    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) {
        SPDLOG_ERROR("Failed to alloc avformat context.");
        return false;
    }

    block_start_time_ = time(nullptr);
    fmt_ctx_->interrupt_callback.callback = OnInterrupt;
    fmt_ctx_->interrupt_callback.opaque = this;

    AVDictionary* dict = InputFmtOptions();
    int error_code = avformat_open_input(&fmt_ctx_, url.c_str(), input_fmt, &dict);
    av_dict_free(&dict);
    if (error_code != 0) {
        SPDLOG_ERROR("Failed to open input stream.");
        return false;
    }

    av_dump_format(fmt_ctx_, 0, url.c_str(), 0);

    int error_code = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (error_code < 0) {
        SPDLOG_ERROR("Failed to find stream information.");
        return false;
    }

    int64_t video_duration = fmt_ctx_->duration / (AV_TIME_BASE / 1000);
    SPDLOG_INFO("Video total time:{0}ms, [{1}].", video_duration,
                QTime::fromMSecsSinceStartOfDay(video_duration).toString("HH:mm:ss zzz").toStdString());

    stream_indexs_[AVMEDIA_TYPE_VIDEO] =
        av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    stream_indexs_[AVMEDIA_TYPE_AUDIO] =
        av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    video_stream_ = fmt_ctx_->streams[stream_indexs_[AVMEDIA_TYPE_VIDEO]];
    fps_ = static_cast<int>(std::round(av_q2d(video_stream_->avg_frame_rate)));

    audio_stream_ = fmt_ctx_->streams[stream_indexs_[AVMEDIA_TYPE_AUDIO]];

    OpenStreamComponent(video_stream_, AVMEDIA_TYPE_VIDEO);
    OpenStreamComponent(audio_stream_, AVMEDIA_TYPE_AUDIO);

    return true;
}

bool FFmpegProcessor::OpenStreamComponent(AVStream* stream, AVMediaType type)
{
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        SPDLOG_ERROR("Failed to find Codec.");
        return false;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        SPDLOG_ERROR("Failed to create video decoder context.");
        return false;
    }

    int error_code = avcodec_parameters_to_context(codec_ctx, stream->codecpar);
    if (error_code < 0) {
        SPDLOG_ERROR("Failed to fill the codec context.");
        return false;
    }

    hw_decode_ =
        Singleton<Config>::Instance()->AppConfigData("video_param", "enable_hw_decode", false).toBool();
    if (hw_decode_) {
        InitHwDecode(codec);
    }

    AVDictionary* dict = nullptr;
    av_dict_set(&dict, "threads", "auto", 0);
    error_code = avcodec_open2(codec_ctx, codec, &dict);
    av_dict_free(&dict);
    if (error_code < 0) {
        SPDLOG_ERROR("Failed to open codec.");
        return false;
    }

    SPDLOG_INFO("resolution: [w:{0}, h:{1}] fps:{2} frames:{3} codec name:{4}",
                stream->codecpar->width, stream->codecpar->height, fps_, stream->nb_frames,
                avcodec_get_name(stream->codecpar->codec_id));

    switch (type) {
    case AVMEDIA_TYPE_VIDEO: {
        video_decoder_ = std::make_unique<VideoDecoder>();
        video_decoder_->Init(codec_ctx, video_queue_, video_frame_queue_);

        video_frame_queue_ = std::make_unique<FrameQueue>(VIDEO_PICTURE_QUEUE_SIZE);
        if (!video_frame_queue_->InitFrameQueue()) {
            return false;
        }
    } break;
    case AVMEDIA_TYPE_AUDIO:
        break;
    default:
        break;
    }

    return true;
}

bool FFmpegProcessor::InputFmt(std::string& url, AVInputFormat** fmt)
{
    switch (media_.type) {
    case kCapture: {
#ifdef _WIN32
        // You can't turn on the webcam if you don't have it on Windows
        *fmt = av_find_input_format("dshow");
#elif __linux__
        *fmt = av_find_input_format("video4linux2");
#else
        *fmt = av_find_input_format("avfoundation");
#endif
        if (!*fmt) {
            SPDLOG_ERROR("Failed to get this input device.");
            return false;
        }

        url = "video=" + media_.src;
        break;
    }
    case kFile:
    case kNetwork: {
        url = media_.src;
        break;
    }
    default:
        SPDLOG_ERROR("Failed to open this media type {0}.", (int)media_.type);
        return false;
    }

    return true;
}

AVDictionary* FFmpegProcessor::InputFmtOptions()
{
    AVDictionary* dict = nullptr;
    if (media_.type == kNetwork) {
        if (strncmp(media_.src.c_str(), "rtsp:", 5) == 0) {
            const char* protocol = Singleton<Config>::Instance()
                                       ->AppConfigData("video_param", "rtsp_transport", "tcp")
                                       .toString()
                                       .toStdString()
                                       .c_str();
            if ((strncmp(protocol, "tcp", 3) != 0) && (strncmp(protocol, "udp", 3) != 0)) {
                protocol = "tcp";
            }
            av_dict_set(&dict, "rtsp_transport", protocol, 0);
        }
        av_dict_set(&dict, "timeout", "5000000", 0); // 5s
    }
    av_dict_set(&dict, "max_delay", "3", 0);
    av_dict_set(&dict, "buffer_size", "2048000", 0);

    return dict;
}

void FFmpegProcessor::InitHwDecode(const AVCodec* codec)
{
    for (int i = 0;; i++) {
        const AVCodecHWConfig* hw_config = avcodec_get_hw_config(codec, i);
        if (!hw_config) {
            SPDLOG_ERROR("Failed to get codec hardware configuration");
            return;
        }

        if (hw_config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) {
            auto iter = std::find(hw_devices_.begin(), hw_devices_.end(), hw_config->device_type);
            if (iter != hw_devices_.end()) {
                int ret =
                    av_hwdevice_ctx_create(&hw_dev_ctx_, hw_config->device_type, nullptr, nullptr, 0);
                if (ret != 0) {
                    SPDLOG_ERROR("Failed to open specified type of hw device or create ctx.");
                    return;
                }

                SPDLOG_INFO("Open the hardware device type as {0}",
                            av_hwdevice_get_type_name(hw_config->device_type));

                codec_ctx_->hw_device_ctx = av_buffer_ref(hw_dev_ctx_);
                codec_ctx_->opaque =
                    static_cast<void*>(const_cast<AVPixelFormat*>(&hw_config->pix_fmt));
                codec_ctx_->get_format = get_hw_format;
                return;
            }
        }
    }
}

AVPixelFormat FFmpegProcessor::GetDstPixFormat()
{
    std::string dst_pix_fmt_str =
        Singleton<Config>::Instance()->AppConfigData("video_param", "dst_pix_fmt").toString().toStdString();

    AVPixelFormat dst_pix_fmt = AV_PIX_FMT_YUV420P;
    if (!dst_pix_fmt_str.empty()) {
        if (dst_pix_fmt_str.compare("YUV") == 0) {
            dst_pix_fmt = AV_PIX_FMT_YUV420P;
        } else if (dst_pix_fmt_str.compare("RGB") == 0) {
            dst_pix_fmt = AV_PIX_FMT_RGB24;
        }
    }

    return dst_pix_fmt;
}

void FFmpegProcessor::AlignSize(int src_w, int src_h, int* dst_w, int* dst_h)
{
    *dst_w = src_w >> 2 << 2;
    *dst_h = src_h;
}

void FFmpegProcessor::ResizeDecodeFrame(int dst_w, int dst_h, int dst_pix_fmt)
{
    // Default alloc size as same as RGBA.
    decode_frame_.buf.resize(dst_w * dst_h * 4);

    decode_frame_.w = dst_w;
    decode_frame_.h = dst_h;
    decode_frame_.format = GetCommonFmt(dst_pix_fmt);

    if (dst_pix_fmt == AV_PIX_FMT_YUV420P) {
        int y_size = dst_w * dst_h;
        decode_frame_.buf.len = y_size * 3 / 2;
        data_[0] = reinterpret_cast<uint8_t*>(decode_frame_.buf.base);
        data_[1] = data_[0] + y_size;
        data_[2] = data_[1] + y_size / 4;
        linesize_[0] = dst_w;
        linesize_[1] = dst_w / 2;
        linesize_[2] = dst_w / 2;
    } else {
        decode_frame_.buf.len = dst_w * dst_h * 3;
        data_[0] = reinterpret_cast<uint8_t*>(decode_frame_.buf.base);
        linesize_[0] = dst_w * 3;
    }
}

bool FFmpegProcessor::GpuDataToCpu(AVFrame* src, AVFrame* dst) const
{
    const AVPixelFormat* format = static_cast<const AVPixelFormat*>(codec_ctx_->opaque);
    if (src->format != *format) {
        return false;
    }

    // int ret = av_hwframe_transfer_data(hw_frame_, frame_, 0);
    int ret = av_hwframe_map(dst, src, 0);
    if (ret != 0) {
        FFmpegHelper::FFmpegError(ret);
        return false;
    }

    dst->width = src->width;
    dst->height = src->height;

    // Copy frame meta data.
    av_frame_copy_props(dst, src);

    return true;
}

bool FFmpegProcessor::Scale(AVFrame* src)
{
    AVPixelFormat dst_pix_fmt = GetDstPixFormat();
    if (!sws_ctx_) {
        int src_w = codec_ctx_->width;
        int src_h = codec_ctx_->height;
        int dst_w;
        int dst_h;
        AlignSize(src_w, src_h, &dst_w, &dst_h);

        sws_ctx_ = sws_getContext(src_w, src_h, static_cast<AVPixelFormat>(src->format), dst_w,
                                  dst_h, dst_pix_fmt, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ctx_) {
            SPDLOG_ERROR("Failed to get sws context.");
            return false;
        }
    }
    int out_h = sws_scale(sws_ctx_, src->data, src->linesize, 0, src->height, data_, linesize_);
    if (out_h <= 0 || out_h != src->height) {
        return false;
    }
    decode_frame_.ts = src->pts * av_q2d(video_stream_->time_base) * 1000;

    return true;
}

void FFmpegProcessor::PushNullPacket(std::shared_ptr<PacketQueue> queue, AVPacket* pkt, int stream_index)
{
    pkt->stream_index = stream_index;
    queue->Push(pkt);
}

AVPixelFormat FFmpegProcessor::get_hw_format(AVCodecContext* ctx, const AVPixelFormat* fmt)
{
    const AVPixelFormat* pix_fmt = static_cast<const AVPixelFormat*>(ctx->opaque);

    for (auto p = fmt; *p != -1; p++) {
        if (*p == *pix_fmt) {
            return *p;
        }
    }

    SPDLOG_ERROR("Failed to get hardware pixel format, AV_PIX_FMT_NONE will return.");
    return AV_PIX_FMT_NONE;
}
