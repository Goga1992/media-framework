from nginx_live_client import *
from kmp_utils import *
import manifest_utils
import os

NGINX_LIVE_HOST = 'localhost'
NGINX_LIVE_URL = 'http://%s:8001' % NGINX_LIVE_HOST
NGINX_LIVE_API_URL = 'http://%s:8001/control' % NGINX_LIVE_HOST
NGINX_LIVE_KMP_ADDR = (NGINX_LIVE_HOST, 6543)

TEST_VIDEO1 = 'video1.mp4'
TEST_VIDEO2 = 'video2.mp4'

TEST_VIDEO_URLS = {
    TEST_VIDEO1: 'http://cdnapi.kaltura.com/p/2035982/playManifest/entryId/0_k13xaap6/flavorId/0_4c84uq72/format/download/a.mp4',
    TEST_VIDEO2: 'http://cdnapi.kaltura.com/p/2035982/playManifest/entryId/0_w4l3m87h/flavorId/0_vsu1xutk/format/download/a.mp4',
}

CHANNEL_ID = 'test'
TIMELINE_ID = 'main'
VARIANT_ID = 'var1'
FILLER_CHANNEL_ID = '__filler'
FILLER_TIMELINE_ID = 'main'

def setupChannelTimeline(channelId):
    nl = NginxLive(NGINX_LIVE_API_URL)
    nl.channel.create(NginxLiveChannel(id=channelId, preset='main'))
    nl.setChannelId(channelId)
    nl.timeline.create(NginxLiveTimeline(id=TIMELINE_ID, active=True))
    return nl

def createTrack(nl, trackName, mediaType, varName=None):
    nl.track.create(NginxLiveTrack(id=trackName, media_type=mediaType))
    if varName is not None:
        nl.variant.addTrack(variantId=varName, trackId=trackName)
    return KmpTcpSender(NGINX_LIVE_KMP_ADDR, nl.channelId, trackName)

def createVariant(nl, varName, tracks):
    nl.variant.create(NginxLiveVariant(id=varName))

    result = []
    for trackName, mediaType in tracks:
        result.append(createTrack(nl, trackName, mediaType, varName))
    return result

def getConfBlock(c, path):
    for cur in c:
        key = cur[0]
        if not isinstance(key, list):
            continue
        if ' '.join(key) != path[0]:
            continue
        if len(path) == 1:
            return cur[1]
        return getConfBlock(cur[1], path[1:])

def testStream(url, basePath, streamName):
    splittedPath = os.path.split(basePath)
    fileName = os.path.splitext(splittedPath[1])[0] + '-%s.txt' % streamName
    filePath = os.path.join(splittedPath[0], 'ref', fileName)

    info = manifest_utils.getStreamInfo(url)
    if not os.path.isfile(filePath):
        print 'Info: saving stream, url: %s, file: %s' % (url, filePath)
        file(filePath, 'w').write(info)
        return

    expected = file(filePath, 'r').read()
    if expected == info:
        return

    newFilePath = filePath + '.new'
    file(newFilePath, 'w').write(info)
    print 'Error: stream does not match, url: %s, orig: %s, new: %s' % (url, filePath, newFilePath)

def getStreamUrl(channelId, prefix, suffix='', timelineId=TIMELINE_ID):
    if len(suffix) == 0 and prefix.startswith('hls'):
        suffix = 'master.m3u8'

    return NGINX_LIVE_URL + '/%s/%s/tl/%s/%s' % (prefix, channelId, timelineId, suffix)

def testDefaultStreams(channelId, basePath):
    for prefix in ['hls-ts', 'hls-fmp4']:
        testStream(getStreamUrl(channelId, prefix), basePath, prefix)
