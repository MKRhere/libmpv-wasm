import libmpvLoader from './libmpv.js';
import _ from 'lodash';
import { isAudioTrack, isVideoTrack } from './utils';
import { MainModule } from './libmpv.js';

type ProxyHandle<K, V> = (this: MpvPlayer, value: V, key: K) => void;
interface ProxyOptions {
    idle: ProxyHandle<'idle', MpvPlayer['idle']>;
    isPlaying: ProxyHandle<'isPlaying', MpvPlayer['isPlaying']>;
    duration: ProxyHandle<'duration', MpvPlayer['duration']>;
    elapsed: ProxyHandle<'elapsed', MpvPlayer['elapsed']>;
    videoStream: ProxyHandle<'videoStream', MpvPlayer['videoStream']>;
    videoTracks: ProxyHandle<'videoTracks', MpvPlayer['videoTracks']>;
    audioStream: ProxyHandle<'audioStream', MpvPlayer['audioStream']>;
    audioTracks: ProxyHandle<'audioTracks', MpvPlayer['audioTracks']>;
    subtitleStream: ProxyHandle<'subtitleStream', MpvPlayer['subtitleStream']>;
    subtitleTracks: ProxyHandle<'subtitleTracks', MpvPlayer['subtitleTracks']>;
    currentChapter: ProxyHandle<'currentChapter', MpvPlayer['currentChapter']>;
    chapters: ProxyHandle<'chapters', MpvPlayer['chapters']>;
    isSeeking: ProxyHandle<'isSeeking', MpvPlayer['isSeeking']>;
    title: ProxyHandle<'title', MpvPlayer['title']>;
    fileEnd: ProxyHandle<'fileEnd', MpvPlayer['fileEnd']>;
    idleActive: ProxyHandle<'idleActive', MpvPlayer['idleActive']>;
    shaderCount: ProxyHandle<'shaderCount', MpvPlayer['shaderCount']>;
    /** Reflects mpv's `paused-for-cache`: true while buffering the stream. */
    buffering: ProxyHandle<'buffering', MpvPlayer['buffering']>;
}

const MPV_PLAYER_PROPERTIES = [
    'idle', 'isPlaying', 'duration', 'elapsed',
    'videoStream', 'videoTracks', 'audioStream', 'audioTracks',
    'subtitleStream', 'subtitleTracks', 'currentChapter', 'chapters',
    'isSeeking', 'title', 'fileEnd', 'idleActive', 'shaderCount', 'buffering',
] as const;

const isMpvPlayerProperty = (prop: string | symbol): prop is keyof MpvPlayer =>
    (MPV_PLAYER_PROPERTIES as readonly (string | symbol)[])
        .includes(typeof prop === 'symbol' ? prop.toString() : prop);

export default class MpvPlayer {
    module: MainModule;

    mpvWorker: Worker | null = null;

    fileEnd = false;
    idle = false;
    idleActive = false;
    isPlaying = false;
    duration = 0;
    elapsed = 0;
    buffering = false;

    videoStream = 1;
    audioStream = 1;
    currentChapter = 0;
    subtitleStream = 1;

    videoTracks: VideoTrack[] = [];
    audioTracks: AudioTrack[] = [];
    subtitleTracks: Track[] = [];
    chapters: Chapter[] = [];

    isSeeking = false;
    title: 0 | string = 0;

    shaderCount = -1;

    proxy: MpvPlayer;

    static vectorToArray<T>(vector: Vector<T>) {
        const arr: T[] = [];
        for (let i = 0; i < vector.size(); i++) {
            const val = vector.get(i);
            if (val === undefined) continue;
            arr.push(val);
        }
        return arr;
    }

    private constructor(module: MainModule, options: Partial<ProxyOptions>) {
        this.module = module;

        this.proxy = new Proxy(this, {
            set(target, prop, newValue) {
                if (!isMpvPlayerProperty(prop))
                    return false;

                if (( prop === 'title' && !['string', 'number'].includes(typeof newValue) )
                    || ( prop !== 'title' && typeof newValue !== typeof target[prop] )
                ) return false;

                // @ts-ignore
                target[prop] = newValue;

                if (options[prop as keyof typeof options]) {
                    // @ts-ignore
                    options[prop].call(target, newValue, prop);
                }

                return true;
            },
        });

        this.setupMpvWorker();
    }

    static async load(
        canvas: HTMLCanvasElement, mainScriptUrlOrBlob: string, options: Partial<ProxyOptions> = {}
    ) {
        const module = await libmpvLoader({
            canvas,
            mainScriptUrlOrBlob
        });

        return new this(module, options);
    }

    async setupMpvWorker() {
        const mainThread = await new Promise<number>(resolve => {
            const interval = setInterval(() => {
                const threadId = this.module.getMpvThread();
                if (!threadId) return;
                clearInterval(interval);
                resolve(threadId);
            }, 100);
        });
        const pthreads: Record<number, Worker> = this.module.PThread.pthreads;
        this.mpvWorker = pthreads[mainThread];
        if (!this.mpvWorker)
            throw new Error('mpv worker not found');

        this.mpvWorker.addEventListener('message', this.handleMessage);
    }

    private handleMessage = (e: MessageEvent) => {
        let payload: any;
        try {
            payload = JSON.parse(e.data);
        } catch {
            return;
        }

        switch (payload.type) {
            case 'idle':
                this.proxy.idle = true;
                this.proxy.shaderCount = payload.shaderCount;
                break;
            case 'file-start':
                this.proxy.fileEnd = false;
                break;
            case 'file-end':
                this.proxy.fileEnd = true;
                break;
            case 'property-change':
                this.handlePropertyChange(payload.name, payload.value);
                break;
            case 'track-list':
                this.handleTrackList(payload.tracks);
                break;
            case 'chapter-list':
                this.proxy.chapters = payload.chapters;
                break;
            default:
                console.log('Received payload:', payload);
        }
    };

    private handlePropertyChange(name: string, value: any) {
        switch (name) {
            case 'pause':
                this.proxy.isPlaying = !value;
                break;
            case 'duration':
                this.proxy.duration = value;
                break;
            case 'playback-time':
                if (this.isSeeking) break;
                this.proxy.elapsed = value;
                break;
            case 'paused-for-cache':
                this.proxy.buffering = Boolean(value);
                break;
            case 'core-idle':
                this.proxy.idleActive = Boolean(value);
                break;
            case 'vid':
                this.proxy.videoStream = parseInt(value);
                break;
            case 'aid':
                this.proxy.audioStream = parseInt(value);
                break;
            case 'sid':
                this.proxy.subtitleStream = parseInt(value);
                break;
            case 'chapter':
                this.proxy.currentChapter = parseInt(value);
                break;
            case 'shaderCount':
                this.proxy.shaderCount = value;
                break;
            case 'metadata/by-key/title':
                this.proxy.title = value;
                break;
            default:
                console.log(`event: property-change -> { name: ${name}, value: ${value} }`);
        }
    }

    private handleTrackList(rawTracks: any[]) {
        const bigIntKeys = [
            'id', 'srcId', 'mainSelection', 'ffIndex',
            'demuxW', 'demuxH', 'demuxChannelCount', 'demuxSamplerate'
        ];

        const tracks = rawTracks
            .map((track: any) => _.mapKeys(track, (__, k) => _.camelCase(k)))
            .map((track: any) => _.mapValues(track, (v, k) => bigIntKeys.includes(k) ? BigInt(v as any) : v)) as unknown as Track[];

        const audioTrackSrcIds: bigint[] = [];

        const { videoTracks, audioTracks, subtitleTracks } = tracks.reduce(
            (map: {
                videoTracks: VideoTrack[],
                audioTracks: AudioTrack[],
                subtitleTracks: Track[]
            }, track) => {
                if (isVideoTrack(track))
                    map.videoTracks.push(track);
                else if (isAudioTrack(track) && !audioTrackSrcIds.includes(track.srcId)) {
                    map.audioTracks.push(track);
                    audioTrackSrcIds.push(track.srcId);
                } else
                    map.subtitleTracks.push(track);

                return map;
            }, {
                videoTracks: [],
                audioTracks: [],
                subtitleTracks: []
            }
        );

        this.proxy.videoTracks = videoTracks;
        this.proxy.audioTracks = audioTracks;
        this.proxy.subtitleTracks = subtitleTracks;
    }

    /* --- Playback controls (thin wrappers over the wasm module) --- */

    loadFile(path: string, options = '') {
        this.module.loadFile(path, options);
    }

    togglePlay() {
        this.module.togglePlay();
    }

    stop() {
        this.module.stop();
    }

    seek(time: number) {
        this.proxy.elapsed = time;
        this.module.setPlaybackTime(time);
    }

    setVolume(volume: number) {
        this.module.setVolume(volume);
    }

    setVideoTrack(id: bigint) {
        this.module.setVideoTrack(id);
    }

    setAudioTrack(id: bigint) {
        this.module.setAudioTrack(id);
    }

    setSubtitleTrack(id: bigint) {
        this.module.setSubtitleTrack(id);
    }

    setChapter(idx: bigint) {
        this.module.setChapter(idx);
    }

    skipForward() {
        this.module.skipForward();
    }

    skipBackward() {
        this.module.skipBackward();
    }

    addShaders() {
        this.module.addShaders();
    }

    clearShaders() {
        this.module.clearShaders();
    }

    matchWindowScreenSize() {
        this.module.matchWindowScreenSize();
    }
}
