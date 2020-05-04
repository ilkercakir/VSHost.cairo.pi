#include "AudioEncoder.h"

int check_sample_fmt(const AVCodec *codec, enum AVSampleFormat sample_fmt)
{
    const enum AVSampleFormat *p = codec->sample_fmts;
    while (*p != AV_SAMPLE_FMT_NONE) {
printf("%d\n", *p);
        if (*p == sample_fmt)
            return 1;
        p++;
    }
    return 0;
}

int init_encoder(audioencoder *aen, char *filename, enum AVCodecID id, enum AVSampleFormat avformat, snd_pcm_format_t format, unsigned int rate, unsigned int channels, int64_t bit_rate)
{
	aen->format = format;
	aen->rate = rate;
	aen->channels = channels;
	aen->bit_rate = bit_rate;

	aen->avformat = avformat;
	aen->id = id;
	aen->codec = NULL;
	aen->codeccontext = NULL;
	aen->formatcontext = NULL;
	aen->stream = NULL;
	aen->iocontext = NULL;

	aen->encoderbuffer = NULL;
	aen->front = aen->rear = 0;

	int err;

	/* register all the codecs */
	av_register_all();
	avcodec_register_all();

	/* Open the output file to write to it. */
	if ((err = avio_open(&(aen->iocontext), filename, AVIO_FLAG_WRITE)) < 0)
	{
		printf("Could not open output file '%s' (err '%s')\n", filename, av_err2str(err));
		return -1;
	}

	/* Create a new format context for the output container format. */
	if (!(aen->formatcontext = avformat_alloc_context()))
	{
		printf("Could not allocate output format context\n");
        return -1;
	}

	err = 0;

	/* Associate the output file (pointer) with the container format context. */
	aen->formatcontext->pb = aen->iocontext;

	/* Guess the desired container format based on the file extension. */
	if (!(aen->formatcontext->oformat = av_guess_format(NULL, filename, NULL)))
	{
		printf("Could not find output file format %s\n", filename);
		err = -1;
	}
	else
	{
		av_strlcpy(aen->formatcontext->filename, filename, sizeof(aen->formatcontext->filename));

		/* Find the encoder to be used by its name. */
		aen->codec = avcodec_find_encoder(aen->id);
		if (!aen->codec)
		{
			printf("Codec %d not found\n", aen->id);
			err = -1;
		}
		else
		{
			//check_sample_fmt(aen->codec, aen->avformat);

			/* Create a new audio stream in the output file container. */
			if (!(aen->stream = avformat_new_stream(aen->formatcontext, NULL)))
			{
				printf("Could not create new stream\n");
				err = -1;
			}
			else
			{
				aen->codeccontext = avcodec_alloc_context3(aen->codec);
				if (!aen->codeccontext)
				{
					printf("Could not allocate audio codec context\n");
					err = -1;
				}
				else
				{
					aen->codeccontext->channels = aen->channels;
					aen->codeccontext->channel_layout = av_get_default_channel_layout(aen->channels);
					aen->codeccontext->sample_rate = aen->rate;
					aen->codeccontext->sample_fmt = aen->avformat;
					aen->codeccontext->bit_rate = aen->bit_rate;

					/* Allow the use of the experimental AAC encoder */
					aen->codeccontext->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

					/* Set the sample rate for the container. */
					aen->stream->time_base.den = aen->rate;
					aen->stream->time_base.num = 1;

					/*
					 * Some container formats (like MP4) require global headers to be present
					 * Mark the encoder so that it behaves accordingly.
					 */
					if (aen->formatcontext->oformat->flags & AVFMT_GLOBALHEADER)
						aen->codeccontext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

					/* Open the encoder for the audio stream to use it later. */
					if ((err = avcodec_open2(aen->codeccontext, aen->codec, NULL)) < 0)
					{
						printf("Could not open output codec (error '%s')\n", av_err2str(err));
						err = -1;
					}
					else
					{
						if ((err = avcodec_parameters_from_context(aen->stream->codecpar, aen->codeccontext)) < 0)
						{
							printf("Could not initialize stream parameters\n");
							err = -1;
						}
						else
						{
							/* Write the header of the output file container. */
							if ((err = avformat_write_header(aen->formatcontext, NULL)) < 0)
							{
								printf("Could not write output file header (error '%s')\n", av_err2str(err));
								err = -1;
							}
							else
							{
								/* frame containing input raw audio */
								aen->frame = av_frame_alloc();
								if (!(aen->frame))
								{
									printf("Could not allocate audio frame\n");
									err = -1;
								}
								else
								{
//printf("frame_size %d\n", aen->codeccontext->frame_size);
									aen->frame->nb_samples = aen->codeccontext->frame_size;
									aen->frame->channels = aen->codeccontext->channels;
									aen->frame->channel_layout = aen->codeccontext->channel_layout;
									aen->frame->format = aen->codeccontext->sample_fmt;
									aen->frame->sample_rate = aen->rate;
									/* allocate the data buffers */
									if ((err = av_frame_get_buffer(aen->frame, 0)) < 0)
									{
										printf("Could not allocate audio data buffers\n");
										err = -1;
									}
									else
									{
										aen->codecframesize = aen->codeccontext->frame_size * aen->channels * ( snd_pcm_format_width(format) / 8 );
										aen->codecbuffer = malloc(aen->codecframesize);
										aen->encoderbuffersize = aen->codecframesize * 10;
										aen->encoderbuffer = malloc(aen->encoderbuffersize);
									}
								}
							}
						}
					}
				}
			}
		}
	}

	if (err)
	{
		avcodec_free_context(&(aen->codeccontext));
		avio_closep(&(aen->formatcontext)->pb);
		avformat_free_context(aen->formatcontext);
		aen->formatcontext = NULL;
	}

	return err;
}

int encoder_encode(audioencoder *aen, char *inbuffer, int buffersize)
{
	AVPacket pkt;
	int ret = 0, data_present, i, j, frame_size;
	
//printf("write %d / %d\n", buffersize, aen->encoderbuffersize);
	// write cq
	int offset, bytesleft, bytestocopy, length;
	for(offset=0,bytesleft=buffersize;bytesleft;bytesleft-=bytestocopy)
	{
		bytestocopy = (aen->rear+bytesleft>aen->encoderbuffersize?aen->encoderbuffersize-aen->rear:bytesleft);
		memcpy(aen->encoderbuffer+aen->rear, inbuffer+offset, bytestocopy);
		offset += bytestocopy;
		aen->rear += bytestocopy; aen->rear %= aen->encoderbuffersize;
	}
//printf("read %d / %d\n", aen->codecframesize, aen->frame->linesize[0]);
	// read cq
	do
	{
		length = (aen->rear - aen->front + aen->encoderbuffersize) % aen->encoderbuffersize;
		if (length<aen->codecframesize)
		{
			break;
		}
		else
		{
			// make sure the frame is writable, makes a copy if the encoder kept a reference internally
			if ((ret = av_frame_make_writable(aen->frame))<0)
			{
				printf("Could not make frame writable (error '%s')\n", av_err2str(ret));
				ret = -1;
			}
			else
			{
				//char *buffer = (char *)aen->frame->data[0];
				for(offset=0,bytesleft=aen->codecframesize;bytesleft;bytesleft-=bytestocopy)
				{
					bytestocopy = (aen->front+bytesleft>aen->encoderbuffersize?aen->encoderbuffersize-aen->front:bytesleft);
					memcpy(aen->codecbuffer+offset, aen->encoderbuffer+aen->front, bytestocopy);
					offset += bytestocopy;
					aen->front += bytestocopy; aen->front %= aen->encoderbuffersize;
				}

				if (av_sample_fmt_is_planar(aen->avformat))
				{
					frame_size = aen->codeccontext->frame_size;
					signed short *src = (signed short *)aen->codecbuffer;
					for(i=0;i<aen->channels;i++)
					{
						signed short *dst = (signed short *)aen->frame->data[i];
						for(j=0;j<frame_size;j++)
						{
							dst[j] = src[j*aen->channels+i];
						}
					}
				}
				else
				{
					memcpy(aen->frame->data[0], aen->codecbuffer, aen->codecframesize);
				}

				av_init_packet(&pkt);
				pkt.data = NULL; // packet data will be allocated by the encoder
				pkt.size = 0;

				if ((ret = avcodec_encode_audio2(aen->codeccontext, &pkt, aen->frame, &data_present)) < 0)
				{
					printf("Could not encode frame (error '%s')\n", av_err2str(ret));
					av_packet_unref(&pkt);
					ret = -1;
				}
				else
				{
					// Write one audio frame from the temporary packet to the output file.
					if (data_present) 
					{
						if ((ret = av_write_frame(aen->formatcontext, &pkt)) < 0)
						{
							printf("Could not write frame (error '%s')\n", av_err2str(ret));
							av_packet_unref(&pkt);
							ret = -1;
						}
						else
							av_packet_unref(&pkt);
					}
				}
			}
		}
		if (ret<0) break;
	}
	while(1);

	return(ret);
}

int close_encoder(audioencoder *aen)
{
	int err;
	if ((err = av_write_trailer(aen->formatcontext)) < 0)
	{
		printf("Could not write output file trailer (error '%s')\n", av_err2str(err));
		err = -1;
	}

	if (aen->codeccontext)
	{
		avcodec_close(aen->codeccontext);
		avcodec_free_context(&(aen->codeccontext));
	}

	if (aen->formatcontext)
		avformat_free_context(aen->formatcontext);

	if (aen->iocontext)
		avio_close(aen->iocontext);

	free(aen->codecbuffer);
	free(aen->encoderbuffer);
	
	return err;
}
