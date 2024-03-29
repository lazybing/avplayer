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

int main(int argc, char **argv) {
    //Initalizing these to NULL prevents segfaults!
    AVFormatContext *pFormatCtx = NULL;
    int i, videoStream;
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

    SDL_Event   event;
    SDL_Window *screen;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    uint8_t *yPlane, *uPlane, *vPlane;
    size_t yPlaneSz, uvPlaneSz;
    int uvPitch;

    if (argc < 2) {
        printf("Please provide a movie file\n");    
        return -1;
    }

    {
        SDL_version compiled;
        SDL_version linked;
        SDL_VERSION(&compiled);
        SDL_GetVersion(&linked);
        printf("SDL version %d.%d.%d\n", compiled.major, compiled.minor, compiled.patch);
        printf("SDL linked version %d.%d.%d\n", linked.major, linked.minor, linked.patch);
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
    for (i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = i;
            break;
        }
    }
    if (videoStream == -1)
        return -1;  //didn't find a video stream

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
        }

        //Free the packet that was allocated by av_read_frame
        av_packet_unref(&packet);
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

    //Close the video file
    avformat_close_input(&pFormatCtx);

    return 0;
}
