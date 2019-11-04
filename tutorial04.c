#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <SDL.h>
#include <SDL_thread.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE  192000

#define MAX_AUDIOQ_SIZE (5* 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 1

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutext *mutex;
    SDL_cond   *cond;
}PacketQueue;

typedef struct VideoPicture {
    //SDL_Overlay *bmp;
    int width, height;  //source height & width
    int allocated;
}VideoPicture;

typedef struct VideoState {
    AVFormatContext *pFormatCtx;
    int             videoStream, audioStream;
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

/*
 * Sine we only have one decoding thread, the Big Struct
 * can be global in case we need it.
 */
VideoState *global_video_state;

void packet_queue_init(PacketQueue *q){
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond  = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacketList *pktl;

    if (av_dup_pcket(pkt) < 0) 
        return -1;
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

    SDL_LockMutext(q->mutex);

    for (;;) {
        if (global_video_state->quit) {
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

int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size) {
    int len1, data_size = 0;
    AVPacket *pkt = &is->audio_pkt;

    for (;;) {
        while (is->audio_pkt_size > 0) {
            int got_frame = 0;
            len1 = avcodec_decode_audio4(is->audio_ctx, &is->audio_frame, &got_frame, pkt);
            if (len1 < 0) {
                //if error, skip frame
                is->audio_pkt_size = 0;
                break;
            }
            data_size = 0;
            if (got_frame) {
                data_size = av_sample_get_buffer_size(NULL,
                                                      is->audio_ctx->channels,
                                                      is->audio_frame.nb_samples,
                                                      is->audio_ctx->sample_fmt,
                                                      1);
                assert(data_size <= buf_size);
                memcpy(audio_buf, is->audio_frame.data[0], data_size);
            }
            is->audio_pkt_data += len1;
            is->audio_pkt_size -= len1;
            if (data_size <= 0) {
                //No data yet, get more frames
                continue;
            }
            // we have data, return it and come back for more later
            return data_size;
        }
        if (pkt->data)
            av_free_packet(pkt);

        if (is->quit) {
            return -1;
        }
        //next packet
        if (packet_queue_get(&is->audioq, pkt, 1) < 0) {
            return -1;
        }
        is->audio_pkt_data = pkt->data;
        is->audio_pkt_size = pkt->size;
    }
}

void audio_callback(void *userdata, uint8_t *stream, int len) {
    VideoState *is = (VideoState *)userdata;
    int len1, audio_size;

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
            //we have already sent all our data; get more
            audio_size = audio_decode_frame(is, is->audio_buf, sizeof(is->audio_buf));
            if (audio_size < 0) {
                //If error, output silence    
                is->audio_buf_size = 1024;
                memset(is->audio_buf, 0, is->audio_buf_size);
            } else {
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
}

static uint8_t sdl_refresh_timer_cb(uint32_t interval, void *opaque) {
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0;   //0 meanse stop timer
}

//schedule a video refresh in 'delay' ms
static void schedule_refresh(VideoState *is, int delay) {
    SDL_AddTimer(delay, sdl_refresh_timer_sb, is);
}

void video_display(VideoState *is) {
    SDL_Rect rect;
    VideoPicture *vp;
    float aspect_ratio;
    int w, h, x, y;
    int i;

    vp = &is->pictq[is->pictq_rindex];
    if (vp->bmp) {
        if (is->video_ctx->sample_aspect_ration.num == 0) {
            aspect_ratio = 0;
        } else {
            aspect_ratio = av_q2d(is->video_ctx->sample_aspect_ratio) *
                is->video_ctx->width / is->video_ctx->height;
        }

        if (aspect_ratio <= 0.0) {
            aspect_ratio = (float)is->video_ctx->width /(float)is->video_ctx->height;
        }
        h = screen->h;
        w = ((int)rint(h * aspect_ratio)) & -3;
        if (w > screen->w) {
            w = screen->w;
            h = ((int)rint(w / aspect_ratio)) & -3;
        }
        x = (screen->w - w) / 2;
        y = (screen->h - h) / 2;

        rect.x = x;
        rect.y = y;
        rect.w = w;
        rect.h = h;
        SDL_LockMutex(screen_mutex);
        SDL_DisplayYUOverlay(vp->bmp, &rect);
        SDL_UnlockMutex(screen_mutex);
    }
}

void video_refresh_timer(void *userdata) {
    VideoState *is = (VideoState *)userdata;
    VideoPicture *vp;

    if (is->video_st) {
        if (is->pictq_size == 0) {
            schedule_refresh(is, 1);
        } else {
            vp = &is->pictq[is->pictq_rindex];
            /*
             * Now, normally here goes a ton of code
             * about timing, etc, we're just going to
             * guess at a delay for now. You can
             * increase and decrease this value and hard code
             * the timing - but i don't sugget that.
             * We'll learn how to do if for real later.
             */
            schedule_refresh(is, 40);

            //show the picture
            video_display(is);

            //update queue for next picture!
            if (++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
                is->pictq_rindex = 0;   
            }
            SDL_LockMutex(is->pictq_mutex);
            is->pictq_size--;
            SDL_CondSignal(is->pictq_mutex);
            SDL_UnlockMutex(is->pictq_mutex);
        }
    } else {
        schedule_refresh(is, 100);
    }
}

void alloc_picture(void *userdata) {
    VideoState *is = (VideoState *)userdata;
    VideoPicture *vp;

    vp = &is->pictq[is->pictq_windex];
    if (vp->bmp) {
        //we already have one make another, bigger/smaller
        SDL_FreeYUVOverlay(vp->bmp);
    }
    //Allocate a place to put our YUV image on that screen
    SDL_LockMutex(screen_mutex);
    vp->bmp = SDL_CreateYUVOverlay(is->video_ctx->width,
                                   is->video_ctx->height,
                                   SDL_YV12_OVERLAY,
                                   screen);
    SDL_UnlockMutex(screen_mutex);

    vp->width = is->video_ctx->width;
    vp->height = is->video_ctx->height;
    vp->allocated = 1;
}

