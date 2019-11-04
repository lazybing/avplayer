//tutorial01.c
//A small sample program that show how to use 
//libavformat and libavcodec to read video from a file
//Use
//gcc -o tutorial01 tutorial01.c -lavformat -lavcodec -lswscale -lz

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#include <SDL.h>
#include <SDL_thread.h>

#include <stdio.h>
#include <assert.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond  *cond;
}PacketQueue;

PacketQueue audioq;

int quit = 0;

void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond  = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacketList *pktl;
    if (av_dup_packet(pkt) < 0) {
        return -1;
    }
    pktl = av_malloc(sizeof(AVPacketList));
    if (!pktl)
        return -1;
    pktl->pkt  = *pkt;
    pktl->next = NULL;

    SDL_LockMutex(q->mutex);

    if (!q->last_pkt)
        q->first_pkt = pktl;
    else
        q->last_pkt->next = pktl;
    q->last_pkt = pktl;
    q->nb_packets++;
    q->size += pktl->pkt.size;
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);

    return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pktl;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {
        if (quit) {
            ret = -1;
            break;
        }

        pktl = q->first_pkt;
        if (pktl) {
            q->first_pkt = pktl->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pktl->pkt.size;
            *pkt = pktl->pkt;
            av_free(pktl);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);

    return ret;
}

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size) {
    static AVPacket pkt;
    static uint8_t *audio_pkt_data = NULL;
    static int audio_pkt_size = 0;
    static AVFrame frame;

    int len1, data_size = 0;

    for (;;) {
        while (audio_pkt_size > 0) {
            int got_frame = 0;
            len1 = avcodec_decode_audio4(aCodecCtx, &frame, &got_frame, &pkt);
            if (len1 < 0) {
                //if error, skip frame
                audio_pkt_size = 0;
                break;
            }
            audio_pkt_data += len1;
            audio_pkt_size -= len1;
            data_size = 0;
            if (got_frame) {
                data_size = av_samples_get_buffer_size(NULL,
                                                       aCodecCtx->channels,
                                                       frame.nb_samples,
                                                       aCodecCtx->sample_fmt,
                                                       1);
                assert(data_size <= buf_size);
                memcpy(audio_buf, frame.data[0], data_size);
            }
            if (data_size <= 0) {
                //no data yet, get more frames
                continue;
            }
            // we have data, return it and come back for more later
            return data_size;
        }
        if (pkt.data)
            av_free_packet(&pkt);
        if (quit) {
            return -1;
        }
        if (packet_queue_get(&audioq, &pkt, 1) < 0) {
            return -1;
        }
        audio_pkt_data = pkt.data;
        audio_pkt_size = pkt.size;
    }
}

void audio_callback(void *userdata, uint8_t *stream, int len) {
    AVCodecContext *aCodecCtx = (AVCodecContext *)userdata;
    int len1, audio_size;

    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;

    while (len > 0) {
        if (audio_buf_index >= audio_buf_size) {
            //we have already sent all our data; get more
            audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
            if (audio_size < 0) {
                //If error, output silence
                audio_buf_size = 1024;
                memset(audio_buf, 0, audio_buf_size);
            } else {
                audio_buf_size = audio_size;
            }
            audio_buf_index = 0;
        }
        len1 = audio_buf_size - audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
        len -= len1;
        stream += len1;
        audio_buf_index += len1;
    }
}

int main(int argc, char **argv) {
    //Initalizing these to NULL prevents segfaults!
    AVFormatContext *pFormatCtx = NULL;
    int i, videoStream, audioStream;
    AVCodecContext  *pCodecCtxOrig = NULL;
    AVCodecContext  *pCodecCtx = NULL;
    AVCodec         *pCodec = NULL;
    AVFrame         *pFrame = NULL;
    AVFrame         *pFrameRGB = NULL;
    AVPacket        packet;
    int             frameFinished;
    int             numBytes;
    uint8_t         *buffer = NULL;
    struct SwsContext *sws_ctx = NULL;

    AVCodecContext *aCodecCtxOrig = NULL;
    AVCodecContext *aCodecCtx = NULL;
    AVCodec        *aCodec = NULL;

    SDL_Event   event;
    SDL_Window *screen;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    uint8_t *yPlane, *uPlane, *vPlane;
    size_t yPlaneSz, uvPlaneSz;
    int uvPitch;
    SDL_AudioSpec wanted_spec, spec;

    if (argc < 2) {
        printf("Please provide a movie file\n");    
        return -1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }

    //open video file
    if (avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0)
        return -1;  //couldn't open file

    //retrieve stream information
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
        return -1;  //couldn't find stream information

    //Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, argv[1], 0);

    //Find the first video stream
    videoStream = -1;
    audioStream = -1;
    for (i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO && videoStream < 0) {
            videoStream = i;
        }
        if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO && audioStream < 0) {
            audioStream = i;
        }
    }
    if (videoStream == -1)
        return -1;  //didn't find a video stream
    if (audioStream == -1)
        return -1;

    aCodecCtxOrig = pFormatCtx->streams[audioStream]->codec;
    aCodec = avcodec_find_decoder(aCodecCtxOrig->codec_id);
    if (!aCodec) {
        fprintf(stderr, "Unsupported audio codec!\n");
        return -1;
    }
    
    //Copy context
    aCodecCtx = avcodec_alloc_context3(aCodec);
    if (avcodec_copy_context(aCodecCtx, aCodecCtxOrig) != 0) {
        fprintf(stderr, "Couldn't copy codec context");
        return -1;
    }

    //Set audio settings from codec info
    wanted_spec.freq    = aCodecCtx->sample_rate;
    wanted_spec.format  = AUDIO_S16SYS;
    wanted_spec.channels = aCodecCtx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = aCodecCtx;

    if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        return -1;
    }

    avcodec_open2(aCodecCtx, aCodec, NULL);

    // audio_st = pFormatCtx->streams[index];
    packet_queue_init(&audioq);
    SDL_PauseAudio(0);

    //Get a pointer to the codec context for the video stream
    pCodecCtxOrig = pFormatCtx->streams[videoStream]->codec;
    //Find the decoder for the video stream
    pCodec = avcodec_find_decoder(pCodecCtxOrig->codec_id);
    if (pCodec == NULL) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;  //codec not found
    }
    //copy context
    pCodecCtx = avcodec_alloc_context3(pCodec);
    if (avcodec_copy_context(pCodecCtx, pCodecCtxOrig) != 0) {
        fprintf(stderr, "Couldn't copy codec context");
        return -1;//Error copying codec context
    }

    //open codec
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0)
        return -1;  //could not open codec

    //Allocate video frame
    pFrame = av_frame_alloc();

    //Make a screen to put our video
    screen = SDL_CreateWindow(
                "FFmpeg Tutorial",
                SDL_WINDOWPOS_UNDEFINED,
                SDL_WINDOWPOS_UNDEFINED,
                pCodecCtx->width,
                pCodecCtx->height,
                0);
    if (!screen) {
        fprintf(stderr, "SDL: could not create widow-exiting\n");
        exit(1);
    }

    renderer = SDL_CreateRenderer(screen, -1, 0);
    if (!renderer) {
        fprintf(stderr, "SDL:could not create renderer -exiting\n");
        exit(1);
    }

    //Allocate a place to put our YUV image on that screen
    texture = SDL_CreateTexture(
                renderer,
                SDL_PIXELFORMAT_YV12,
                SDL_TEXTUREACCESS_STREAMING,
                pCodecCtx->width,
                pCodecCtx->height);
    if (!texture) {
        fprintf(stderr, "SDL: could not create texture - exiting\n");
        exit(1);
    }

    //initialize SWS context for software scaling
    sws_ctx = sws_getContext(pCodecCtx->width,
                             pCodecCtx->height,
                             pCodecCtx->pix_fmt,
                             pCodecCtx->width,
                             pCodecCtx->height,
                             AV_PIX_FMT_YUV420P,
                             SWS_BILINEAR,
                             NULL,
                             NULL,
                             NULL);

    //set up YV12 pixel array(12 bits per pixel)
    yPlaneSz = pCodecCtx->width * pCodecCtx->height;
    uvPlaneSz = yPlaneSz >> 2;
    yPlane = (uint8_t *)malloc(yPlaneSz);
    uPlane = (uint8_t *)malloc(uvPlaneSz);
    vPlane = (uint8_t *)malloc(uvPlaneSz);
    if (!yPlane || !uPlane || !vPlane) {
        fprintf(stderr, "Could not allocate pixel buffers - exiting\n");
        exit(1);
    }

    //Read frames and save first five five frames to disk
    uvPitch = pCodecCtx->width >> 1;
    while (av_read_frame(pFormatCtx, &packet) >= 0) {
        //Is this a packet from the video stream?
        if (packet.stream_index == videoStream) {
            //Decode video frame
            avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);

            //Did we get a video frame?
            if (frameFinished) {
                AVPicture pict;
                pict.data[0] = yPlane;
                pict.data[1] = uPlane;
                pict.data[2] = vPlane;

                pict.linesize[0] = pCodecCtx->width;
                pict.linesize[1] = uvPitch;
                pict.linesize[2] = uvPitch;

                //Convert the image from its native format to RGB
                sws_scale(sws_ctx, (uint8_t const *const *)pFrame->data,
                          pFrame->linesize, 0, pCodecCtx->height,
                          pict.data, pict.linesize);

                SDL_UpdateYUVTexture(
                                     texture,
                                     NULL,
                                     yPlane,
                                     pCodecCtx->width,
                                     uPlane,
                                     uvPitch,
                                     vPlane,
                                     uvPitch);
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, NULL, NULL);
                SDL_RenderPresent(renderer);
            }
        } else if (packet.stream_index == audioStream) {
            packet_queue_put(&audioq, &packet);
        } else {
            av_free_packet(&packet);
        }

        //Free the packet that was allocated by av_read_frame
        //av_packet_unref(&packet);
        SDL_PollEvent(&event);
        switch(event.type) {
            case SDL_QUIT:
                SDL_Quit();
                exit(0);
                break;
            default:
                break;
        }
    }

    //Free the YUV frame
    av_frame_free(&pFrame);

    //Close the codecs
    avcodec_close(pCodecCtx);
    avcodec_close(pCodecCtxOrig);
    avcodec_close(aCodecCtx);
    avcodec_close(aCodecCtxOrig);

    //Close the video file
    avformat_close_input(&pFormatCtx);

    return 0;
}
