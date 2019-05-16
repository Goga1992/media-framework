//
//  sender.c
//  live_transcoder
//
//  Created by Guy.Jacubovski on 21/03/2019.
//  Copyright © 2019 Kaltura. All rights reserved.
//

#include "core.h"
#include "utils.h"
#include "logger.h"

#include "kalturaMediaProtocol.h"
#include "KMP.h"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <regex.h>
#include <netdb.h>
#include <unistd.h> // close function
#include "libavutil/intreadwrite.h"

#define DUMMY_NOPTS_VALUE -999999
int KMP_connect( KMP_session_t *context,char* url)
{
    
    int ret=0;
    
    char host[256];
    char port[5];
    
    int n=sscanf(url,"kmp://%255[^:]:%5s ",host,port);// this line isnt working properly
    if (n!=2) {
        LOGGER(CATEGORY_KMP,AV_LOG_FATAL,"Cannot parse url '%s'",url);
        return 0;
    }

    struct addrinfo hints, *servinfo;
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; //ipv4!
    hints.ai_socktype = SOCK_STREAM; //tcp
    hints.ai_flags = AI_CANONNAME;
    
    if ((ret = getaddrinfo(host, port, &hints, &servinfo)) != 0) {
        LOGGER(CATEGORY_KMP,AV_LOG_FATAL,"Cannot resolve %s - error %d (%s)",url,ret,av_err2str(ret));
        return -1;
    }
    struct addrinfo *p=servinfo; //take first match

    if ((ret = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) <= 0)
    {
        LOGGER(CATEGORY_KMP,AV_LOG_FATAL,"Socket creation for %s - error %d (%s)",url,ret,av_err2str(ret));
        return ret;
    }
    context->socket=ret;

    if ( (ret=connect(context->socket,p->ai_addr, p->ai_addrlen)) < 0)
    {
        LOGGER(CATEGORY_KMP,AV_LOG_FATAL,"Connection Failed for %s - error %d (%s)",url,ret,av_err2str(ret));
        return ret;
    }
    return 1;
}


static kmp_codec_id get_audio_codec(AVCodecParameters *apar)
{
    switch (apar->codec_id) {
        case AV_CODEC_ID_AAC:
            return KMP_CODEC_AUDIO_AAC;
        case AV_CODEC_ID_MP3:
            return KMP_CODEC_AUDIO_MP3;
        default:
            return (kmp_codec_id)apar->codec_id;
    }
}

static kmp_codec_id get_video_codec(AVCodecParameters *apar)
{
    switch (apar->codec_id) {
        case AV_CODEC_ID_FLV1:
            return KMP_CODEC_VIDEO_SORENSON_H263;
        case AV_CODEC_ID_FLASHSV:
            return KMP_CODEC_VIDEO_SCREEN;
        case AV_CODEC_ID_FLASHSV2:
            return KMP_CODEC_VIDEO_SCREEN2;
        case AV_CODEC_ID_VP6F:
            return KMP_CODEC_VIDEO_ON2_VP6;
        case AV_CODEC_ID_VP6A:
            return KMP_CODEC_VIDEO_ON2_VP6_ALPHA;
        case AV_CODEC_ID_H264:
            return KMP_CODEC_VIDEO_H264;
        default:
            return (kmp_codec_id)apar->codec_id;
    }
}

int KMP_send_header( KMP_session_t *context,transcode_mediaInfo_t* mediaInfo )
{
    if (context->socket==0)
    {
        LOGGER0(CATEGORY_KMP,AV_LOG_FATAL,"Invalid socket");
        return -1;
    }
    AVCodecParameters *codecpar=mediaInfo->codecParams;
    kmp_packet_header_t header;
    kmp_media_info_t media_info;
    header.packet_type=KMP_PACKET_MEDIA_INFO;
    header.header_size=sizeof(kmp_packet_header_t)+sizeof(media_info);
    header.data_size=codecpar->extradata_size;
    media_info.bitrate=(uint32_t)codecpar->bit_rate;
    media_info.timescale=mediaInfo->timeScale.den;
    if (codecpar->codec_type==AVMEDIA_TYPE_VIDEO)
    {
        media_info.media_type=KMP_MEDIA_VIDEO;
        media_info.codec_id=get_video_codec(codecpar);
        media_info.u.video.width=codecpar->width;
        media_info.u.video.height=codecpar->height;
        media_info.u.video.frame_rate.denom=mediaInfo->frameRate.den;
        media_info.u.video.frame_rate.num=mediaInfo->frameRate.num;
    }
    if (codecpar->codec_type==AVMEDIA_TYPE_AUDIO)
    {
        media_info.media_type=KMP_MEDIA_AUDIO;
        media_info.codec_id=get_audio_codec(codecpar);
        media_info.u.audio.bits_per_sample=codecpar->bits_per_raw_sample;
        media_info.u.audio.sample_rate=codecpar->sample_rate;
        media_info.u.audio.channels=codecpar->channels;
    }
    
    send(context->socket , &header , sizeof(header) , 0 );
    send(context->socket , &media_info , sizeof(media_info) , 0 );
    if (codecpar->extradata_size>0) {
        send(context->socket , codecpar->extradata , codecpar->extradata_size , 0 );
    }
    
    return 0;
}



static const uint8_t *kk_avc_find_startcode_internal(const uint8_t *p, const uint8_t *end)
{
    const uint8_t *a = p + 4 - ((intptr_t)p & 3);
    
    for (end -= 3; p < a && p < end; p++) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
            return p;
    }
    
    for (end -= 3; p < end; p += 4) {
        uint32_t x = *(const uint32_t*)p;
        //      if ((x - 0x01000100) & (~x) & 0x80008000) // little endian
        //      if ((x - 0x00010001) & (~x) & 0x00800080) // big endian
        if ((x - 0x01010101) & (~x) & 0x80808080) { // generic
            if (p[1] == 0) {
                if (p[0] == 0 && p[2] == 1)
                    return p;
                if (p[2] == 0 && p[3] == 1)
                    return p+1;
            }
            if (p[3] == 0) {
                if (p[2] == 0 && p[4] == 1)
                    return p+2;
                if (p[4] == 0 && p[5] == 1)
                    return p+3;
            }
        }
    }
    
    for (end += 3; p < end; p++) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
            return p;
    }
    
    return end + 3;
}

const uint8_t *kk_avc_find_startcode(const uint8_t *p, const uint8_t *end){
    const uint8_t *out= kk_avc_find_startcode_internal(p, end);
    if(p<out && out<end && !out[-1]) out--;
    return out;
}


uint32_t kk_avc_parse_nal_units( const uint8_t *buf_in, int size,int socket)
{
    
    const uint8_t *p = buf_in;
    const uint8_t *end = p + size;
    const uint8_t *nal_start=NULL, *nal_end=NULL;
    uint32_t nNalSize;
    uint32_t written = 0;
    nal_start = kk_avc_find_startcode(p, end);
    for (;;) {
        while (nal_start < end && !*(nal_start++));
        if (nal_start == end)
            break;
        
        nal_end = kk_avc_find_startcode(nal_start, end);
        nNalSize = (uint32_t)(nal_end - nal_start);
        
        if (socket!=0) {
            send(socket, &nNalSize, sizeof(uint32_t), 0);
            send(socket, nal_start, nNalSize, 0);
        }
        written += sizeof(uint32_t) + nNalSize;
        nal_start = nal_end;
    }
    return written;
}




int KMP_send_packet( KMP_session_t *context,AVPacket* packet)
{
    bool annex_b=
        AV_RB32(packet->data) == 0x00000001 ||
        AV_RB24(packet->data) == 0x000001;
    
    kmp_packet_header_t packetHeader;
    kmp_frame_t sample;
    
    packetHeader.packet_type=KMP_PACKET_FRAME;
    packetHeader.header_size=sizeof(sample)+sizeof(packetHeader);
    packetHeader.data_size = annex_b ? kk_avc_parse_nal_units(packet->data,packet->size,0) : packet->size;
    if (AV_NOPTS_VALUE!=packet->pts) {
        sample.pts_delay=(uint32_t)(packet->pts - packet->dts);
    } else {
        sample.pts_delay=DUMMY_NOPTS_VALUE;
    }
    sample.dts=packet->dts;
    sample.created=packet->pos;
    sample.flags=((packet->flags& AV_PKT_FLAG_KEY)==AV_PKT_FLAG_KEY)? KMP_FRAME_FLAG_KEY : 0;

    
    send(context->socket, &packetHeader, sizeof(packetHeader), 0);
    send(context->socket, &sample, sizeof(sample), 0);
    if (annex_b) {
        kk_avc_parse_nal_units(packet->data, packet->size,context->socket);
    } else {
        send(context->socket, packet->data, packet->size, 0);
    }
    return 0;
}


int KMP_send_eof( KMP_session_t *context)
{
    kmp_packet_header_t packetHeader;
    packetHeader.packet_type=KMP_PACKET_EOS;
    packetHeader.header_size=0;
    packetHeader.data_size=0;
    send(context->socket, &packetHeader, sizeof(packetHeader), 0);
    
    return 0;
}

int KMP_send_handshake( KMP_session_t *context,const char* channel_id,const char* track_id)
{
    kmp_connect_header_t connect;
    connect.header.packet_type=KMP_PACKET_CONNECT;
    connect.header.header_size=sizeof(kmp_connect_header_t);
    connect.header.data_size=0;
    strcpy((char*)connect.channel_id,channel_id);
    strcpy((char*)connect.track_id,track_id);
    send(context->socket, &connect, sizeof(connect), 0);

    return 0;
}

int KMP_close( KMP_session_t *context)
{
    close(context->socket);
    context->socket=0;
    return 0;
}

int KMP_listen( KMP_session_t *context)
{
    int ret=0;
    // Creating socket file descriptor
    if ((ret = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) <= 0)
    {
        LOGGER(CATEGORY_KMP,AV_LOG_FATAL,"Socket creation error %d (%s)",ret,av_err2str(ret));
        return ret;
    }
    
    context->socket =ret;
    
    /*
     if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
     &opt, sizeof(opt)))
     {
     perror("setsockopt");
     exit(EXIT_FAILURE);
     }*/
    context->address.sin_family = AF_INET;
    context->address.sin_addr.s_addr = INADDR_ANY;
    context->address.sin_port = htons( context->listenPort );
    
    // Forcefully attaching socket to the port
    if ( (ret=bind(context->socket, (struct sockaddr *)&context->address,sizeof(context->address)))<0)
    {
        LOGGER(CATEGORY_KMP,AV_LOG_FATAL,"bind to port %d failed  error:%d (%s)",context->listenPort,ret,av_err2str(ret));
        return ret;
    }
    if ( (ret=listen(context->socket, 10)) < 0)
    {
        LOGGER(CATEGORY_KMP,AV_LOG_FATAL,"listen failed %d (%s)",ret,av_err2str(ret));
        return ret;
    }
    
    socklen_t addrLen = sizeof(context->address);
    if (getsockname(context->socket, (struct sockaddr *)&context->address, &addrLen) == -1) {
        LOGGER0(CATEGORY_KMP,AV_LOG_FATAL,"getsockname() failed");
        return -1;
    }
    
    context->listenPort=htons( context->address.sin_port );
    
    return 0;
}

int KMP_accept( KMP_session_t *context, KMP_session_t *client)
{
    int addrlen = sizeof(context->address);
    int clientSocket=accept(context->socket, (struct sockaddr *)&context->address,
                            (socklen_t*)&addrlen);
    
    if (clientSocket<=0) {
        return clientSocket;
    }
    client->socket =clientSocket;
    client->listenPort=0;
    return 1;
}


int recvExact(int socket,char* buffer,int bytesToRead) {
    
    if (bytesToRead==0) {
        LOGGER(CATEGORY_KMP,AV_LOG_FATAL,"!!!!recvExact invalid bytesToRead=  %d",bytesToRead);

    }
    int bytesRead=0;
    while (bytesToRead>0) {
        int valread = (int)recv(socket,buffer+bytesRead, bytesToRead, 0);
        if (valread<=0){
            LOGGER(CATEGORY_KMP,AV_LOG_FATAL,"incomplete recv, returned %d",valread);
            return valread;
        }
        bytesRead+=valread;
        bytesToRead-=valread;
    }
    return bytesRead;
}


int KMP_read_header( KMP_session_t *context,kmp_packet_header_t *header)
{
    int valread =recvExact(context->socket,(char*)header,sizeof(kmp_packet_header_t));
    return valread;
}
int KMP_read_handshake( KMP_session_t *context,kmp_packet_header_t *header,char* channel_id,char* track_id)
{
    kmp_connect_header_t connect;
    if (header->packet_type!=KMP_PACKET_CONNECT) {
        LOGGER(CATEGORY_KMP,AV_LOG_FATAL,"invalid packet, expceted PACKET_TYPE_HANDSHAKE received packet_type=%d",header->packet_type);
        return -1;
    }
    int valread =recvExact(context->socket,((char*)&connect)+sizeof(kmp_packet_header_t),sizeof(kmp_connect_header_t)-sizeof(kmp_packet_header_t));
    if (valread<=0) {
        return valread;
    }
    
    strcpy(channel_id,(char*)connect.channel_id);
    strcpy(track_id,(char*)connect.track_id);
    return 0;
}


static void set_audio_codec(int codecid,AVCodecParameters *apar)
{
    switch (codecid) {
            // no distinction between S16 and S8 PCM codec flags
        case KMP_CODEC_AUDIO_UNCOMPRESSED:
            apar->codec_id = apar->bits_per_coded_sample == 8 ? AV_CODEC_ID_PCM_U8
#if HAVE_BIGENDIAN
            : AV_CODEC_ID_PCM_S16BE;
#else
            : AV_CODEC_ID_PCM_S16LE;
#endif
            break;
        case KMP_CODEC_AUDIO_LINEAR_LE:
            apar->codec_id = apar->bits_per_coded_sample == 8
            ? AV_CODEC_ID_PCM_U8
            : AV_CODEC_ID_PCM_S16LE;
            break;
        case KMP_CODEC_AUDIO_AAC:
            apar->codec_id = AV_CODEC_ID_AAC;
            break;
        case KMP_CODEC_AUDIO_ADPCM:
            apar->codec_id = AV_CODEC_ID_ADPCM_SWF;
            break;
        case KMP_CODEC_AUDIO_SPEEX:
            apar->codec_id    = AV_CODEC_ID_SPEEX;
            apar->sample_rate = 16000;
            break;
        case KMP_CODEC_AUDIO_MP3:
            apar->codec_id      = AV_CODEC_ID_MP3;
            break;
        case KMP_CODEC_AUDIO_NELLY8:
            // in case metadata does not otherwise declare samplerate
            apar->sample_rate = 8000;
            apar->codec_id    = AV_CODEC_ID_NELLYMOSER;
            break;
        case KMP_CODEC_AUDIO_NELLY16:
            apar->sample_rate = 16000;
            apar->codec_id    = AV_CODEC_ID_NELLYMOSER;
            break;
        case KMP_CODEC_AUDIO_NELLY:
            apar->codec_id = AV_CODEC_ID_NELLYMOSER;
            break;
        case KMP_CODEC_AUDIO_G711U:
            apar->sample_rate = 8000;
            apar->codec_id    = AV_CODEC_ID_PCM_MULAW;
            break;
        case KMP_CODEC_AUDIO_G711A:
            apar->sample_rate = 8000;
            apar->codec_id    = AV_CODEC_ID_PCM_ALAW;
            break;
        default:
            apar->codec_tag = codecid;
            break;
    }
}

static void set_video_codec(int codecid,AVCodecParameters *apar)
{
    
    switch (codecid) {
        case KMP_CODEC_VIDEO_SORENSON_H263:
            apar->codec_id = AV_CODEC_ID_FLV1;
            break;
        case KMP_CODEC_VIDEO_SCREEN:
            apar->codec_id = AV_CODEC_ID_FLASHSV;
            break;
        case KMP_CODEC_VIDEO_SCREEN2:
            apar->codec_id = AV_CODEC_ID_FLASHSV2;
            break;
        case KMP_CODEC_VIDEO_ON2_VP6:
            apar->codec_id = AV_CODEC_ID_VP6F;
            break;
        case KMP_CODEC_VIDEO_ON2_VP6_ALPHA:
            apar->codec_id = AV_CODEC_ID_VP6A;
            break;
        case KMP_CODEC_VIDEO_H264:
            apar->codec_id = AV_CODEC_ID_H264;
            break;
        default:
            apar->codec_tag = codecid;
            break;
    }
}

int KMP_read_mediaInfo( KMP_session_t *context,kmp_packet_header_t *header,transcode_mediaInfo_t *transcodeMediaInfo)
{
    kmp_media_info_t mediaInfo;
    if (header->packet_type!=KMP_PACKET_MEDIA_INFO) {
        LOGGER(CATEGORY_KMP,AV_LOG_FATAL,"invalid packet, expceted PACKET_TYPE_HEADER received packet_type=%d",header->packet_type);
        return -1;
    }
    int valread =recvExact(context->socket,(char*)&mediaInfo,sizeof(kmp_media_info_t));
    if (valread<=0) {
        return valread;
    }
    AVCodecParameters* params=transcodeMediaInfo->codecParams;
    if (mediaInfo.media_type==KMP_MEDIA_AUDIO) {
        params->codec_type=AVMEDIA_TYPE_AUDIO;
        params->sample_rate=mediaInfo.u.audio.sample_rate;
        params->bits_per_raw_sample=mediaInfo.u.audio.bits_per_sample;
        params->channels=mediaInfo.u.audio.channels;
        params->channel_layout=av_get_default_channel_layout(params->channels);
        set_audio_codec(mediaInfo.codec_id,params);
    }
    if (mediaInfo.media_type==KMP_MEDIA_VIDEO) {
        params->codec_type=AVMEDIA_TYPE_VIDEO;
        params->format=AV_PIX_FMT_YUV420P;
        params->width=mediaInfo.u.video.width;
        params->height=mediaInfo.u.video.height;
        transcodeMediaInfo->frameRate.den=mediaInfo.u.video.frame_rate.denom;
        transcodeMediaInfo->frameRate.num=mediaInfo.u.video.frame_rate.num;
        set_video_codec(mediaInfo.codec_id,params);
    }
    transcodeMediaInfo->timeScale.den=mediaInfo.timescale;
    transcodeMediaInfo->timeScale.num=1;
    params->bit_rate=mediaInfo.bitrate;
    //params->codec_id=mediaInfo.codec_id;
    params->extradata_size=header->data_size;
    params->extradata=NULL;
    if (params->extradata_size>0) {
        params->extradata=av_mallocz(params->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        valread =recvExact(context->socket,(char*)params->extradata,header->data_size);
        if (valread<=0) {
            return valread;
        }
    }
    
    return 1;
    
}

int KMP_readPacket( KMP_session_t *context,kmp_packet_header_t *header,AVPacket *packet)
{
    kmp_frame_t sample;
    
    int valread =recvExact(context->socket,(char*)&sample,sizeof(kmp_frame_t));
    if (valread<=0){
        return valread;
    }
    
    av_new_packet(packet,(int)header->data_size);
    packet->dts=sample.dts;
    if (sample.pts_delay!=DUMMY_NOPTS_VALUE) {
        packet->pts=sample.dts+sample.pts_delay;
    } else {
        packet->pts=AV_NOPTS_VALUE;
    }
    packet->duration=0;
    packet->pos=sample.created;
    packet->flags=((sample.flags& KMP_FRAME_FLAG_KEY )==KMP_FRAME_FLAG_KEY)? AV_PKT_FLAG_KEY : 0;
    
    valread =recvExact(context->socket,(char*)packet->data,(int)header->data_size);
    return valread;
}
