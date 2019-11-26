#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <SDL.h>
#include <SDL_thread.h>

#include <stdio.h>
#include <assert.h>
#include <math.h>

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 28.1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free  avcodec_free_frame
#endif

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 1

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond  *cond;
}PacketQueue;

typedef struct VideoPicture {
    SDL_Overlay *bmp;
    int width, height;  //source height & width
    int allocated;
    double pts;
}VideoPicture;

typedef struct VideoState {
    AVFormatContext *pFormatCtx;
    int videoStream, audioStream;

    double audio_clock;
    AVStream        *audio_st;
    AVCodecContext  *audio_ctx;
    PacketQueue     audioq;
    uint8_t         audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];
    unsigned int    audio_buf_size;
    unsigned int    audio_buf_index;
    AVFrame         audio_frame;
    AVPacket        audio_pkt;
    uint8_t         *audio_pkt_data;
    int             audio_pkt_size;
    int             audio_hw_buf_size;
    double          frame_timer;
    double          frame_last_pts;
    double          frame_last_delay;
    double          video_clock;    ///<pts of last decoded frame / predicted pts of next 
    AVStream        *video_st;
    AVCodecContext  *video_ctx;
    PacketQueue     videoq;
    struct SwsContext *sws_ctx;

    VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];
    int             pictq_size, pictq_rindex, pictq_windex;
    SDL_mutex       *pictq_mutex;
    SDL_cond        *pictq_cond;

    SDL_Thread      *parse_tid;
    SDL_Thread      *video_tid;

    char            filename[1024];
    int             quit;
}VideoState;

SDL_Surface         *screen;
SDL_mutex           *screen_mutex;

/*
 *  Since we only have one decoding thread, the Big Struct
 *  can ba global in case we need it.
 * */
VideoState *global_video_state;

int stream_component_open(VideoState *is, int stream_index) {
    AVFormatContext *pFormatCtx = is->pFormatCtx;
    AVCodecContext *codecCtx = NULL;
    AVCodec *codec = NULL;
    SDL_AudioSpec wanted_spec, spec;

    if (stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
        return -1;
    }

    codec = avcodec_find_decoder(pFormatCtx->streams[stream_index]->codec->codec_id);
    if (!codec) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }

    codecCtx = avcodec_alloc_context3(codec);
    if (avcodec_copy_context(codecCtx, pFormatCtx->streams[stream_index]->codec) != 0) {
        fprintf(stderr, "Couldnot copy codec context");
        return -1;
    }

    if (codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
        // set audio settings from codec info
        wanted_spec.freq        =   codecCtx->sample_rate;
        wanted_spec.format      =   AUDIO_S16SYS;
        wanted_spec.channels    =   codecCtx->channels;
        wanted_spec.silence     =   0;
        wanted_spec.samples     =   SDL_AUDIO_BUFFER_SIZE;
        wanted_spec.callback    =   audio_callback;
        wanted_spec.userdata    =   is;

        if (SDL_OpenAudio(&wanted_spce, &spec) < 0) {
            fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
            return -1;
        }
        is->audio_hw_buf_size = spec.size;
    }

    if (avcodec_open2(codecCtx, codec, NULL) < 0) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }

    switch (codecCtx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            is->audioStream = stream_index;
            is->audio_st    = pFormatCtx->streams[stream_index];
            is->audio_ctx   = codecCtx;
            is->audio_buf_size = 0;
            is->audio_buf_index = 0;
            memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
            packet_queue_init(&is->audioq);
            SDL_PauseAudio(0);
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->videoSteam  = stream_index;
            is->video_st    = pFormatCtx->streams[stream_index];
            is->video_ctx   = codecCtx;

            is->frame_timer = (double)av_gettime() / 10000000.0;
            is->frame_last_delay = 40e-3;

            packet_queue_init(&is->videoq);
            is->video_tid = SDL_CreateThread(video_thread, is);
            is->sws_ctx = sws_getContext(is->video_ctx->width, is->video_ctx->height,
                                         is->video_ctx->pix_fmt, is->video_ctx->width,
                                         is->video_ctx->height, PIX_FMT_YUV420P,
                                         SWS_BILINEAR, NULL, NULL, NULL);
            break;
        default:
            break;
    }
}

int decode_thread(void *arg) {
    VideoState *is = (VideoState *)arg;
    AVFormatContext *pFormatCtx;
    AVPacket pkt1, *packet = &pkt1;

    int video_index = -1;
    int audio_index = -1;
    int i;

    is->videoStream = -1;
    is->audioStream = -1;

    global_video_state = is;

    //Open vidoe file
    if (avformat_open_input(&pFormatCtx, is->filename, NULL, NULL) != 0)
        return -1;  //Couldn't open file

    is->pFormatCtx = pFormatCtx;

    // Retrieve stream information
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
        return -1;  //Couldn't find stream information

    //Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, is->filename, 0);

    //Find the first video stream
    for (i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO &&
            video_index < 0) {
            video_index = i;
        }
        if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO &&
            audio_index < 0) {
            audio_index= i;
        }
    }
    if (audio_index >= 0) {
        stream_component_open(is, audio_index);
    }
    if (video_index >= 0) {
        stream_component_open(is, video_index);
    }

    if (is->videoStream < 0 || is->audioStream < 0) {
        printf(stderr, "%s: could not open codecs\n", is->filename);
        goto fail;
    }

    //main decode loop
    for (;;) {
        if (is->quit) {
            break;
        }
        //seek stuff goes here
        if (is->audioq.size > MAX_AUDIOQ_SIZE ||
            is->audioq.size > MAX_VIDEOQ_SIZE) {
            SDL_Delay(10);
            continue;
        }
        if (av_read_frame(is->pFormatCtx, packet) < 0) {
            if (is->pFormatCtx->pb->error == 0) {
                SDL_Delay(100); //no error, wait for user input
                continue;
            } else {
                break;
            }
        }

        //Is this a packet from the video stream?
        if (packet->stream_index == is->videoStream) {
            packet_queue_put(&is->videoq, packet);
        } else if (packet->stream_index == is->audioStream) {
            packet_queue_put(&is->audioq, packet);
        } else {
            av_free_packet(packet);
        }
    }

    //all done -- wait for it
    while (!is->quit) {
        SDL_Delay(100);
    }

fail:
    if (1) {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }

    return 0;
}

int main(int argc, char **argv) {
    SDL_Event event;
    VideoState *is;

    if (argc < 2) {
        fprintf(stderr, "Usage: test <file>\n");
        exit(1);
    }

    is = av_mallocz(sizeof(VideoState));

    //register all formats and codecs
    av_register_all();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }

    //make a screen to put our video
#ifndef __DARWIN__
    screen = SDL_SetVideoMode(640, 480, 0, 0);
#else
    screen = SDL_SetVideoMode(640, 480, 24, 0);
#endif
    if (!screen) {
        fprintf(stderr, "SDL: could not set video mode - exiting\n");
        exit(1);
    }

    screen_mutex = SDL_CreateMutex();

    av_strlcpy(is->filename, argv[1], sizeof(is->filename));

    is->pictq_mutex = SDL_CreateMutex();
    is->pictq_cond  = SDL_CreateCond();

    schedule_refresh(is, 40);

    is->parse_tid = SDL_CreateThread(decode_thread, is);
    if (!is->parse_tid) {
        av_free(is);
        return -1;
    }

    for (;;) {
        SDL_WaitEvent(&event);
        switch(event.type) {
        case FF_QUIT_EVENT:
        case SDL_QUIT:
            is->quit = 1;
            SDL_Quit();
            return 0;
            break;
        case FF_REFRESH_EVENT:
            video_refresh_timer(event.user.data1);
            break;
        default:
            break;
        }
    }

    return 0;
}

