/*
 * VideoPlayer.c
 * 
 * Copyright 2017  <pi@raspberrypi>
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 * 
 * 
 */

#include "VSTVideoPlayer.h"

typedef struct
{
	long long usecs; // microseconds
}videoplayertimer;

long get_first_usec(videoplayertimer *t)
{
	long long micros;
    struct timespec spec;

    clock_gettime(CLOCK_REALTIME, &spec);

	micros = spec.tv_sec * 1.0e6 + round(spec.tv_nsec / 1000); // Convert nanoseconds to microseconds
	t->usecs = micros;
	return(0L);
}

long get_next_usec(videoplayertimer *t)
{
    long delta;
    long long micros;
    struct timespec spec;

    clock_gettime(CLOCK_REALTIME, &spec);

    micros = spec.tv_sec * 1.0e6 + round(spec.tv_nsec / 1000); // Convert nanoseconds to microseconds
    delta = micros - t->usecs;
    t->usecs = micros;
    return(delta);
}

gboolean invalidate(gpointer data)
{
	videoplayer *v = (videoplayer*)data;

	GdkWindow *dawin = gtk_widget_get_window(v->drawingarea);
	cairo_region_t *region = gdk_window_get_clip_region(dawin);
	gdk_window_invalidate_region (dawin, region, TRUE);
	//gdk_window_process_updates (dawin, TRUE);
	cairo_region_destroy(region);
	return FALSE;
}

void init_videoplayer(videoplayer *v, int width, int height, int vqMaxLength, audiopipe *ap, int thread_count)
{
	int ret;

	v->playerWidth = width;
	v->playerHeight = height;
	if (!v->playerHeight)
		v->playerHeight = v->playerWidth * 9 / 16;

	if((ret=pthread_mutex_init(&(v->seekmutex), NULL))!=0 )
		printf("seek mutex init failed, %d\n", ret);

	if((ret=pthread_cond_init(&(v->pausecond), NULL))!=0 )
		printf("pause cond init failed, %d\n", ret);

	if((ret=pthread_mutex_init(&(v->framemutex), NULL))!=0 )
		printf("frame mutex init failed, %d\n", ret);

	v->catalog_folder = NULL;

	for(ret=0;ret<4;ret++)
	{
		CPU_ZERO(&(v->cpu[ret]));
		CPU_SET(ret, &(v->cpu[ret]));
	}

	v->vqMaxLength = vqMaxLength;

	vq_init(&(v->vpq), vqMaxLength);
	v->stoprequested = 0;

	v->ap = ap;
	v->spk_samplingrate = ap->rate;

	v->thread_count = thread_count;
	v->pFormatCtx = NULL;
	v->pCodecCtx = NULL;
	v->pCodecCtxA = NULL;
	v->sws_ctx = NULL;
	v->pCodec = NULL;
	v->pCodecA = NULL;
	v->optionsDict = NULL;
	v->optionsDictA = NULL;
}

void close_videoplayer(videoplayer *v)
{
	avformat_close_input(&(v->pFormatCtx));

	vq_destroy(&(v->vpq));
	pthread_mutex_destroy(&(v->framemutex));
	pthread_cond_destroy(&(v->pausecond));
	pthread_mutex_destroy(&(v->seekmutex));
}

void requeststop_videoplayer(videoplayer *v)
{
	vq_requeststop(&(v->vpq));
}

void signalstop_videoplayer(videoplayer *v)
{
	vq_signalstop(&(v->vpq));
}

void drain_videoplayer(videoplayer *v)
{
	vq_drain(&(v->vpq));
}

int open_now_playing(videoplayer *v)
{
	int i;
	char err[200];

	/* FFMpeg stuff */
	v->optionsDict = NULL;
	v->optionsDictA = NULL;

	av_register_all();
	avformat_network_init();

	if(avformat_open_input(&(v->pFormatCtx), v->now_playing, NULL, NULL)!=0)
	{
		printf("avformat_open_input()\n");
		return -1; // Couldn't open file
	}

    // Retrieve stream information
	if(avformat_find_stream_info(v->pFormatCtx, NULL)<0)
	{
		printf("avformat_find_stream_info()\n");
		return -1; // Couldn't find stream information
	}
  
	// Dump information about file onto standard error
	//av_dump_format(v->pFormatCtx, 0, v->now_playing, 0);

	// Find the first video stream
    v->videoStream=-1;
	for(i=0; i<v->pFormatCtx->nb_streams; i++)
	{
		if (v->pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO)
		{
			v->videoStream=i;
			break;
		}
	}
	if (v->videoStream==-1)
	{
//printf("No video stream found\n");
		v->videoduration = 0;
//		return -1; // Didn't find a video stream
	}
	else
	{
		// Get a pointer to the codec context for the video stream
		v->pCodecCtx=v->pFormatCtx->streams[v->videoStream]->codec;

		// Find the decoder for the video stream
		v->pCodec=avcodec_find_decoder(v->pCodecCtx->codec_id);
		if (v->pCodec==NULL)
		{
			printf("Unsupported video codec\n");
			return -1; // Codec not found
		}

		v->pCodecCtx->thread_count = v->thread_count;
		// Open codec
		if (avcodec_open2(v->pCodecCtx, v->pCodec, &(v->optionsDict))<0)
		{
			printf("Video avcodec_open2()\n");
			return -1; // Could not open video codec
		}

		AVStream *st = v->pFormatCtx->streams[v->videoStream];
		if (st->avg_frame_rate.den)
		{
			v->frame_rate = st->avg_frame_rate.num / (double)st->avg_frame_rate.den;
			v->frametime = 1000000 / v->frame_rate; // usec
		}
		else
		{
			v->frame_rate = 0;
			v->frametime = 1000000; // 1s
		}
//printf("Frame rate = %2.2f\n", frame_rate);
//printf("frametime = %d usec\n", frametime);
//printf("Width : %d, Height : %d\n", v->pCodecCtx->coded_width, v->pCodecCtx->coded_height);
		v->videoduration = (v->pFormatCtx->duration / AV_TIME_BASE) * v->frame_rate; // in frames

		switch(v->pCodecCtx->pix_fmt)
		{
			case AV_PIX_FMT_YUYV422:
			case AV_PIX_FMT_YUV422P:
			case AV_PIX_FMT_YUVJ422P:
				v->yuvfmt = YUV422;
				break;
			case AV_PIX_FMT_YUV420P:
				v->yuvfmt = YUV420;
				break;
			case AV_PIX_FMT_RGBA:
			case AV_PIX_FMT_YUVJ420P:
			default:
				v->yuvfmt = RGBA;
				break;
		}
//printf("Pixel format %d\n", v->pCodecCtx->pix_fmt);
	}
	v->audioStream = -1;
	for(i=0; i<v->pFormatCtx->nb_streams; i++)
	{
		if (v->pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO)
		{
			v->audioStream = i;
			break;
		}
	}
	if (v->audioStream==-1)
	{
//printf("No audio stream found\n");
//		return -1; // Didn't find an audio stream
	}
	else
	{
		// Get a pointer to the codec context for the audio stream
		v->pCodecCtxA=v->pFormatCtx->streams[v->audioStream]->codec;

		// Find the decoder for the audio stream
		v->pCodecA=avcodec_find_decoder(v->pCodecCtxA->codec_id);

		if (v->pCodecA==NULL)
		{
			printf("Unsupported audio codec!\n");
			return -1; // Codec not found
		}

		// Open codec
		if (avcodec_open2(v->pCodecCtxA, v->pCodecA, &(v->optionsDictA))<0)
		{
			printf("Audio avcodec_open2()\n");
			return -1; // Could not open audio codec
		}

		// Set up SWR context once you've got codec information
		v->swr = swr_alloc();
/*
		if (v->pCodecCtxA->channel_layout)
		{
			av_opt_set_int(v->swr, "in_channel_layout", v->pCodecCtxA->channel_layout, 0);
			av_opt_set_int(v->swr, "out_channel_layout", v->pCodecCtxA->channel_layout,  0);
		}
*/
		av_opt_set_int(v->swr, "in_channel_count", v->pCodecCtxA->channels, 0);
		av_opt_set_int(v->swr, "out_channel_count", 2,  0);
//printf("%d %d\n", v->pCodecCtxA->sample_rate, v->spk_samplingrate);
		av_opt_set_int(v->swr, "in_sample_rate", v->pCodecCtxA->sample_rate, 0);
		av_opt_set_int(v->swr, "out_sample_rate", v->spk_samplingrate, 0);

		av_opt_set_sample_fmt(v->swr, "in_sample_fmt", v->pCodecCtxA->sample_fmt, 0);
		av_opt_set_sample_fmt(v->swr, "out_sample_fmt", AV_SAMPLE_FMT_S16,  0);
		if ((i=swr_init(v->swr)))
		{
			av_strerror(i, err, sizeof(err));
			printf("swr_init error %d %s\n", i, err);
		}
		v->sample_rate = v->pCodecCtxA->sample_rate;
		v->audioduration = (v->pFormatCtx->duration / AV_TIME_BASE) * v->sample_rate; // in samples
		//printf("audio frame count %lld\n", audioduration);
		v->audioduration = v->audioduration * v->spk_samplingrate / v->sample_rate; // resampler correction
/*
		int64_t last_ts = pFormatCtx->duration / AV_TIME_BASE;
		last_ts = (last_ts * (pFormatCtx->streams[audioStream]->time_base.den)) / (pFormatCtx->streams[audioStream]->time_base.num);
		printf("timebase last_ts %lld\n", last_ts);
*/
	}

	if (v->optionsDict)
		av_dict_free(&(v->optionsDict));
	if (v->optionsDictA)
		av_dict_free(&(v->optionsDictA));

	gdk_threads_add_idle(set_upper, v->vpwp);

	return 0;
}

void request_stop_frame_reader(videoplayer *v)
{
	v->stoprequested = 1;
}

void frame_reader_loop(videoplayer *v)
{
	videoplayertimer vt;

    AVFrame *pFrame = NULL;
    pFrame=av_frame_alloc();

    AVPacket *packet;

    packet=av_malloc(sizeof(AVPacket));
    av_init_packet(packet);
    packet->data = NULL;
    packet->size = 0;

	v->now_decoding_frame = v->now_playing_frame = v->audioframe = 0;
    int64_t fnum=0, aperiod=0;

	char *rgba;
	int width, height, frameFinished, ret;

	uint8_t **dst_data;
	int dst_bufsize;
	int line_size;
	int src_nb_samples = 1024, dst_nb_samples, max_dst_nb_samples;

	max_dst_nb_samples = dst_nb_samples = av_rescale_rnd(src_nb_samples, v->spk_samplingrate, v->pCodecCtxA->sample_rate, AV_ROUND_UP);
	ret = av_samples_alloc_array_and_samples(&dst_data, &line_size, 2, v->spk_samplingrate, AV_SAMPLE_FMT_S16, 0);

	if (v->videoStream!=-1)
	{
		//width = v->pCodecCtx->width;
		//height = v->pCodecCtx->height;
		width = v->codedWidth = v->pCodecCtx->coded_width;
		height = v->codedHeight = v->pCodecCtx->coded_height; 
		v->codecWidth = v->pCodecCtx->width;
		v->codecHeight = v->pCodecCtx->height;
	}

	get_first_usec(&vt);
	while ((av_read_frame(v->pFormatCtx, packet)>=0) && (!v->stoprequested))
	{
		v->diff1=get_next_usec(&vt); //printf("av_read_frame %ld\n", v->diff1);
		//if (!(v->now_playing_frame%10))
		//	gdk_threads_add_idle(setLevel1, (void*)v->vpwp);

		if (packet->stream_index==v->videoStream) 
		{
			//if (v->decodevideo)
			//{
				get_first_usec(&vt);
				if ((ret=avcodec_decode_video2(v->pCodecCtx, pFrame, &frameFinished, packet))<0) // Decode video frame
					printf("Error decoding video frame %d\n", ret);
				v->diff2=get_next_usec(&vt); //printf("%lu usec avcodec_decode_video2\n", v->diff2);
				//if (!(v->now_playing_frame%10))
				//	gdk_threads_add_idle(setLevel2, (void*)v->vpwp);

				if (frameFinished)
				{
					if (!v->videoduration) // no video stream
					{
						AVFrame *pFrameRGB = NULL;
						pFrameRGB = av_frame_alloc();
						av_frame_unref(pFrameRGB);

						get_first_usec(&vt);

						uint8_t *rgbbuffer = NULL;
						int numBytes = avpicture_get_size(AV_PIX_FMT_RGB32, v->playerWidth, v->playerHeight);
						//printf("numbytes %d\n", numBytes);

						rgbbuffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
						avpicture_fill((AVPicture *)pFrameRGB, rgbbuffer, AV_PIX_FMT_RGB32, v->playerWidth, v->playerHeight);

						struct SwsContext *sws_ctx = sws_getContext(v->pCodecCtx->width, v->pCodecCtx->height, v->pCodecCtx->pix_fmt,
						v->playerWidth, v->playerHeight, AV_PIX_FMT_RGB32, SWS_BILINEAR, NULL, NULL, NULL);

						//printf("sws_scale %d %d\n", playerWidth, playerHeight);
						sws_scale(sws_ctx, (uint8_t const* const*)pFrame->data, pFrame->linesize, 0, v->pCodecCtx->height, pFrameRGB->data, pFrameRGB->linesize);
						//printf("sws_scale done\n");
						sws_freeContext(sws_ctx);

						v->diff3=get_next_usec(&vt);
						//gdk_threads_add_idle(setLevel3, (void*)v->vpwp);

						width = v->lineWidth = pFrameRGB->linesize[0] / 4;
						height = v->playerHeight;
						//printf("%d %d, %d %d\n", width, height, v->playerWidth, v->playerHeight);
						rgba = malloc(pFrameRGB->linesize[0] * v->playerHeight);
						//printf("malloc %d %d\n", pFrameRGB->linesize[0], playerHeight);
						memcpy(rgba, pFrameRGB->data[0], pFrameRGB->linesize[0] * v->playerHeight);
						//printf("memcpy %d\n", pFrameRGB->linesize[0] * v->playerHeight);

						av_free(rgbbuffer);
						//printf("av_free\n");
						av_frame_unref(pFrameRGB);
						//printf("av_frame_unref\n");
						av_frame_free(&pFrameRGB);
					}
					else
					{
						switch(v->yuvfmt)
						{
							case YUV422:
								width = v->lineWidth = pFrame->linesize[0];
								rgba = malloc(width*height*2);
								memcpy(&rgba[0], pFrame->data[0], width*height); //y
								memcpy(&rgba[width*height], pFrame->data[1], width*height/2); //u
								memcpy(&rgba[width*height*3/2], pFrame->data[2], width*height/2); //v
								break;
							case YUV420:
							default:
								width = v->lineWidth = pFrame->linesize[0];
								rgba = malloc(width * height*3/2);
								memcpy(&rgba[0], pFrame->data[0], width*height); //y
								memcpy(&rgba[width*height], pFrame->data[1], width*height/4); //u
								memcpy(&rgba[width*height*5/4], pFrame->data[2], width*height/4); //v
//printf("linesizes %d %d %d\n", pFrame->linesize[0], pFrame->linesize[1], pFrame->linesize[2]);
						}
					}

					pthread_mutex_lock(&(v->framemutex));
					if (!v->videoduration)
						fnum = v->now_playing_frame;
					else
						fnum = v->now_decoding_frame++;
					pthread_mutex_unlock(&(v->framemutex));

					vq_add(&(v->vpq), rgba, fnum); //printf("vq added %ld %ld\n", v->videoduration, fnum);

					//if (!(v->now_playing_frame%10))
					//	gdk_threads_add_idle(setLevel9, (void*)v->vpwp);

					av_frame_unref(pFrame);
//printf("frame %lld\n", fnum);

					pthread_mutex_lock(&(v->seekmutex));
					while (v->vpq.playerstatus==PAUSED)
					{
						pthread_cond_wait(&(v->pausecond), &(v->seekmutex));
					}
					pthread_mutex_unlock(&(v->seekmutex));
				}
			//}
		}
		else if (packet->stream_index==v->audioStream)
		{
//printf("audioStream %lld\n", aperiod);
			if ((ret = avcodec_decode_audio4(v->pCodecCtxA, pFrame, &frameFinished, packet)) < 0)
				printf("Error decoding audio frame %d\n", ret);
//printf("avcodec_decode_audio4\n");
			if (frameFinished)
			{
				dst_nb_samples = av_rescale_rnd(swr_get_delay(v->swr, v->pCodecCtxA->sample_rate) + pFrame->nb_samples, v->spk_samplingrate, v->pCodecCtxA->sample_rate, AV_ROUND_UP);
//printf("nb/max = %d/%d\n", dst_nb_samples, max_dst_nb_samples);
				if (dst_nb_samples > max_dst_nb_samples)
				{
					av_freep(&dst_data[0]);
					if ((ret = av_samples_alloc(dst_data, &line_size, 2, dst_nb_samples, AV_SAMPLE_FMT_S16, 1))<0)
					{
						printf("av_samples_alloc error %d\n", ret);
						break;
					}
					max_dst_nb_samples = dst_nb_samples;
				}

				// convert to destination format
				if ((ret = swr_convert(v->swr, dst_data, dst_nb_samples, (const uint8_t **)pFrame->extended_data, pFrame->nb_samples))<0)
					printf("Error while converting %d\n", ret);
				dst_bufsize = av_samples_get_buffer_size(&line_size, 2, ret, AV_SAMPLE_FMT_S16, 1);
//printf("ret=%d, dst_bufsize=%d\n", ret, dst_bufsize);
				if (dst_data)
				{
					if (dst_data[0])
					{
						if (dst_bufsize)
						{
							aperiod++;
							//printf("bufsize %d\n", dst_bufsize);
							audioCQ_add(v->ap, (char *)dst_data[0], dst_bufsize);

							v->audioframe += v->ap->inbufferframes;
							if (!v->videoduration) // no video stream
							{
								pthread_mutex_lock(&(v->framemutex));
								v->now_playing_frame += v->ap->inbufferframes;
								pthread_mutex_unlock(&(v->framemutex));

								if (!(aperiod%10))
									gdk_threads_add_idle(update_hscale, (void*)v->vpwp);
							}
						}
					}
				}
				av_frame_unref(pFrame);

				pthread_mutex_lock(&(v->seekmutex));
				while (v->vpq.playerstatus==PAUSED)
				{
					pthread_cond_wait(&(v->pausecond), &(v->seekmutex));
				}
				pthread_mutex_unlock(&(v->seekmutex));
			}
		}

		av_packet_unref(packet);
		av_free(packet);
		packet=av_malloc(sizeof(AVPacket));
		av_init_packet(packet);
		packet->data = NULL;
		packet->size = 0;

		get_first_usec(&vt);
	}

	if (dst_data[0])
		av_freep(&(dst_data[0]));
	if (dst_data)
		av_freep(&dst_data);

	av_frame_free(&pFrame);
//printf("av_frame_free\n");
	av_packet_unref(packet);
//printf("av_packet_unref\n");
	av_free(packet);
//printf("av_free packet\n");
	if (v->videoStream!=-1)
	{
		avcodec_flush_buffers(v->pCodecCtx);
		avcodec_close(v->pFormatCtx->streams[v->videoStream]->codec);
		//av_free(pCodecCtx);
	}
	if (v->audioStream!=-1)
	{
		avcodec_flush_buffers(v->pCodecCtxA);
		avcodec_close(v->pFormatCtx->streams[v->audioStream]->codec);
		//av_free(pCodecCtxA);
	}
	avformat_close_input(&(v->pFormatCtx));
	avformat_network_deinit();

	swr_close(v->swr);
	swr_free(&(v->swr));
}

void frame_player_loop(videoplayer *v)
{
	videoqueue *q;
	long diff = 0;
	videoplayertimer vt;
	int width, height;

	v->framesskipped = 0;
	//resetLevels(v);
//printf("starting frame player\n");

	while ((q=vq_remove(&(v->vpq))))
	{
//printf("vq_remove %ld\n", q->label);
//printf("length %d\n", v->vpq.vqLength);
		v->now_playing_frame = q->label;
		if (!v->now_playing_frame) // initialize gl with first frame
		{
			if (v->videoStream!=-1)
			{
				if (!v->videoduration)
				{
					width = v->playerWidth;
					height = v->playerHeight;
				}
				else
				{
					//width = v->pCodecCtx->width;
					//height = v->pCodecCtx->height;
					width = v->codedWidth;
					height = v->codedHeight;
				}
			}
			else
			{
				width = v->playerWidth;
				height = v->playerHeight;
			}

			//printf("reinit fmt width %d height %d, coded width %d coded height %d, codec width %d codec height %d\n", width, height, v->codedWidth, v->codedHeight, v->codecWidth, v->codecHeight);
			oglidle_thread_close_ogl(&(v->oi));
			oglidle_thread_init_ogl(&(v->oi), v->yuvfmt, width, height, v->lineWidth, v->codecHeight);
		}

		//if (!(q->label%10))
		//	gdk_threads_add_idle(setLevel9, &(v->vpq.vqLength));

		if (diff > 0)
		{
			diff-=v->frametime;
			if (diff<0)
			{
				//printf("usleep %ld\n", 0-diff);
				usleep(0-diff);
				diff = 0;
			}
//printf("skip %lld\n", q->label);
			//v->framesskipped++;
			//v->diff7 = (float)(v->framesskipped*100)/(float)v->now_playing_frame;
			//gdk_threads_add_idle(setLevel7, (void*)v->vpwp);

			free(q->yuv);
			free(q);
			continue;
		}

		get_first_usec(&vt);
		oglidle_thread_tex2d_ogl(&(v->oi), q->yuv);
		free(q->yuv);
		free(q);
		v->diff3=get_next_usec(&vt);
//printf("%lu usec glTexImage2D\n", diff3);

		get_first_usec(&vt);
		oglidle_thread_draw_widget(&(v->oi));
		v->diff4=get_next_usec(&vt);
//printf("%lu usec redraw\n", diff4);

		get_first_usec(&vt);
		oglidle_thread_readpixels_ogl(&(v->oi));
		v->diff5=get_next_usec(&vt);
//printf("%lu usec readpixels\n", diff5);

		gdk_threads_add_idle(invalidate, (void*)v);

		if (!(v->now_playing_frame%10))
		{
			//gdk_threads_add_idle(setLevel3,(void*)v->vpwp);
			//gdk_threads_add_idle(setLevel4, (void*)v->vpwp);
			gdk_threads_add_idle(update_hscale, (void*)v->vpwp);
		}

		diff = v->diff3 + v->diff4 + v->diff5 + 1000;
//printf("%5lu usec frame, frametime %5d\n", diff, frametime);
		diff -= v->frametime;
		if (diff<0)
		{
			//printf("usleep %ld\n", 0-diff);
			usleep(0-diff);
			diff = 0;
		}
	}
}

static void* read_frames(void* args)
{
	int ctype = PTHREAD_CANCEL_ASYNCHRONOUS;
	int ctype_old;
	pthread_setcanceltype(ctype, &ctype_old);

	videoplayer *vp = (videoplayer *)args;

	frame_reader_loop(vp);
//printf("requeststop_videoplayer\n");
	requeststop_videoplayer(vp);
//printf("drain_videoplayer\n");
	drain_videoplayer(vp);

//printf("exiting 1, read_frames\n");
	vp->retval_thread1 = 0;
	pthread_exit(&(vp->retval_thread1));
}

static void* videoPlayFromQueue(void* args)
{
	int ctype = PTHREAD_CANCEL_ASYNCHRONOUS;
	int ctype_old;
	pthread_setcanceltype(ctype, &ctype_old);

	videoplayer *vp = (videoplayer *)args;

	frame_player_loop(vp);

	signalstop_videoplayer(vp);

//printf("exiting 2, videoPlayFromQueue\n");
	vp->retval_thread2 = 0;
	pthread_exit(&(vp->retval_thread2));
}

gpointer thread0_videoplayer(void* args)
{
	int ctype = PTHREAD_CANCEL_ASYNCHRONOUS;
	int ctype_old;
	pthread_setcanceltype(ctype, &ctype_old);

	videoplayer *vp = (videoplayer *)args;
	vp->hscaleupd = 1;

	int err;
	err = pthread_create(&(vp->tid[1]), NULL, &read_frames, (void*)vp);
	if (err)
	{}
//printf("read_frames->%d\n", 3);
	if ((err = pthread_setaffinity_np(vp->tid[1], sizeof(cpu_set_t), &(vp->cpu[1]))))
	{
		//printf("pthread_setaffinity_np error %d\n", err);
	}

	err = pthread_create(&(vp->tid[2]), NULL, &videoPlayFromQueue, (void*)vp);
	if (err)
	{}
//printf("videoPlayFromQueue->%d\n", 2);
	if ((err = pthread_setaffinity_np(vp->tid[2], sizeof(cpu_set_t), &(vp->cpu[2]))))
	{
		//printf("pthread_setaffinity_np error %d\n", err);
	}

	int i;
	if ((i=pthread_join(vp->tid[1], NULL)))
		printf("pthread_join error, vp->tid[1], %d\n", i);

	if ((i=pthread_join(vp->tid[2], NULL)))
		printf("pthread_join error, vp->tid[2], %d\n", i);

	vp->hscaleupd = 0;
	close_videoplayer(vp);

//printf("exiting 0, thread0_videoplayer\n");
	vp->retval_thread0 = 0;
	pthread_exit(&(vp->retval_thread0));
}
