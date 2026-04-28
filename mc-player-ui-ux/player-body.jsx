// MC Player — the actual player UI inside the Windows chrome.
// One component tree, rendered with a theme + state. Covers:
//   empty | connecting | playing | error

const { useState: useStatePB, useEffect: useEffectPB, useMemo: useMemoPB } = React;

// ─── Format helpers ────────────────────────────────────────────────────
const pad2 = (n) => String(Math.floor(n)).padStart(2, '0');
const fmtTime = (ms) => {
  const s = Math.floor(ms / 1000);
  return `${pad2(s / 3600)}:${pad2((s % 3600) / 60)}:${pad2(s % 60)}`;
};

// ─── URL input bar ─────────────────────────────────────────────────────
function UrlBar({ theme, url, onChange, onPlay, onStop, state, disabled }) {
  const isPlaying = state === 'playing';
  const isConnecting = state === 'connecting';
  const isError = state === 'error';

  return (
    <div style={{
      display: 'flex', alignItems: 'center', gap: 10,
      padding: '10px 14px',
      background: theme.chrome.urlBg,
      borderBottom: `1px solid ${theme.chrome.border}`,
      position: 'relative', zIndex: 2,
    }}>
      {/* Protocol tag */}
      <div style={{
        display: 'flex', alignItems: 'center', gap: 6,
        padding: '6px 10px', borderRadius: 6,
        background: 'rgba(255,255,255,0.04)',
        border: `1px solid ${theme.chrome.border}`,
        fontFamily: theme.mono, fontSize: 10.5, letterSpacing: 0.8,
        color: theme.fgDim, textTransform: 'uppercase',
      }}>
        <span style={{
          width: 6, height: 6, borderRadius: '50%',
          background: isPlaying ? theme.ok : isError ? theme.err : isConnecting ? theme.warn : theme.mute,
          boxShadow: isPlaying ? `0 0 8px ${theme.ok}` : 'none',
          animation: isConnecting ? 'mc-blink 1s infinite' : 'none',
        }}/>
        rtsp
      </div>

      {/* URL field */}
      <div style={{
        flex: 1, display: 'flex', alignItems: 'center', gap: 8,
        padding: '0 12px', height: 34,
        background: 'rgba(0,0,0,0.35)',
        border: `1px solid ${theme.chrome.border}`,
        borderRadius: 8,
        boxShadow: `inset 0 1px 0 rgba(255,255,255,0.02)`,
      }}>
        <div style={{ color: theme.mute, opacity: 0.8 }}><Icon {...I.link} d={I.link} size={13}/></div>
        <input
          value={url}
          onChange={(e) => onChange && onChange(e.target.value)}
          placeholder="rtsp://admin:pass@192.168.1.64:554/Streaming/Channels/101"
          spellCheck={false}
          disabled={disabled}
          style={{
            flex: 1, background: 'transparent', border: 'none', outline: 'none',
            color: theme.fg, fontFamily: theme.mono, fontSize: 12.5,
            letterSpacing: 0.1, padding: 0,
          }}
        />
        {url && (
          <button style={{
            background: 'transparent', border: 'none', color: theme.mute, cursor: 'pointer',
            padding: 2, display: 'flex',
          }}><Icon d={I.copy} size={12}/></button>
        )}
      </div>

      {/* Primary action — gradient-filled when ready to play */}
      {!isPlaying && !isConnecting ? (
        <button onClick={onPlay} disabled={!url} style={{
          height: 34, padding: '0 18px', borderRadius: 8, border: 'none',
          background: url
            ? `linear-gradient(135deg, ${theme.grad.from}, ${theme.grad.to})`
            : 'rgba(255,255,255,0.04)',
          color: url ? theme.grad.onGrad : theme.mute,
          fontFamily: theme.ui, fontSize: 12, fontWeight: 600, letterSpacing: 0.4,
          cursor: url ? 'pointer' : 'not-allowed',
          display: 'flex', alignItems: 'center', gap: 7,
          boxShadow: url
            ? `0 0 0 1px ${theme.grad.from}33, 0 6px 20px -4px ${theme.grad.to}55`
            : 'none',
          transition: 'transform 0.1s',
        }}>
          <Icon d={I.play} size={12}/> 播放
        </button>
      ) : (
        <button onClick={onStop} style={{
          height: 34, padding: '0 18px', borderRadius: 8,
          border: `1px solid ${theme.chrome.border}`,
          background: 'rgba(255,255,255,0.03)', color: theme.fg,
          fontFamily: theme.ui, fontSize: 12, fontWeight: 500, letterSpacing: 0.4,
          cursor: 'pointer', display: 'flex', alignItems: 'center', gap: 7,
        }}>
          <Icon d={I.stop} size={12}/> {isConnecting ? '取消' : '停止'}
        </button>
      )}

      {/* Settings */}
      <button style={{
        width: 34, height: 34, borderRadius: 8,
        border: `1px solid ${theme.chrome.border}`,
        background: 'rgba(255,255,255,0.03)', color: theme.fgDim,
        display: 'flex', alignItems: 'center', justifyContent: 'center',
        cursor: 'pointer',
      }}><Icon d={I.gear} size={14}/></button>
    </div>
  );
}

// ─── Latency dial ──────────────────────────────────────────────────────
// A giant number, radial gauge behind, with gradient stroke. This is THE
// hero metric.
function LatencyHero({ theme, ms, target = 100 }) {
  const size = 220;
  const r = 96;
  const c = 2 * Math.PI * r;
  // Range 0..300ms mapped to arc
  const pct = Math.min(1, ms / 300);
  const gradId = `hero-grad-${theme.id}`;
  const healthy = ms <= 130;
  return (
    <div style={{ position: 'relative', width: size, height: size }}>
      <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`} style={{ transform: 'rotate(-90deg)' }}>
        <defs>
          <linearGradient id={gradId} x1="0" y1="0" x2="1" y2="1">
            <stop offset="0" stopColor={theme.grad.from}/>
            <stop offset="0.55" stopColor={theme.grad.mid || theme.grad.to}/>
            <stop offset="1" stopColor={theme.grad.to}/>
          </linearGradient>
        </defs>
        {/* track */}
        <circle cx={size/2} cy={size/2} r={r}
                stroke="rgba(255,255,255,0.05)" strokeWidth={6} fill="none"/>
        {/* progress */}
        <circle cx={size/2} cy={size/2} r={r}
                stroke={`url(#${gradId})`} strokeWidth={6} fill="none"
                strokeLinecap="round"
                strokeDasharray={`${pct * c} ${c}`}
                style={{ filter: `drop-shadow(0 0 6px ${theme.grad.to}88)`, transition: 'stroke-dasharray 0.3s' }}/>
        {/* target tick at 100ms (33% of arc) */}
        {[100, 130, 200].map(t => {
          const tpct = t / 300;
          const a = -Math.PI/2 + tpct * 2*Math.PI;
          const x1 = size/2 + Math.cos(a) * (r - 10);
          const y1 = size/2 + Math.sin(a) * (r - 10);
          const x2 = size/2 + Math.cos(a) * (r + 10);
          const y2 = size/2 + Math.sin(a) * (r + 10);
          return <line key={t} x1={x1} y1={y1} x2={x2} y2={y2}
                       stroke={t === 100 ? theme.ok : t === 130 ? theme.warn : theme.err}
                       strokeWidth={1.2} opacity={0.5}/>;
        })}
      </svg>
      {/* Center readout */}
      <div style={{
        position: 'absolute', inset: 0,
        display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center',
      }}>
        <div style={{
          fontSize: 10, letterSpacing: 2, color: theme.mute,
          fontFamily: theme.mono, textTransform: 'uppercase', marginBottom: 4,
        }}>glass-to-glass</div>
        <div style={{
          fontFamily: theme.mono, fontSize: 64, fontWeight: 700,
          lineHeight: 1, letterSpacing: -2,
          fontVariantNumeric: 'tabular-nums',
        }}>
          <GradText theme={theme}>{ms}</GradText>
        </div>
        <div style={{
          fontSize: 11, letterSpacing: 3, color: theme.fgDim,
          fontFamily: theme.mono, marginTop: 4, fontWeight: 500,
        }}>MS</div>
        <div style={{
          marginTop: 10, fontSize: 9.5, letterSpacing: 1.5,
          color: healthy ? theme.ok : theme.warn,
          fontFamily: theme.mono, fontWeight: 600,
          display: 'flex', alignItems: 'center', gap: 4,
        }}>
          <span style={{
            width: 4, height: 4, borderRadius: '50%',
            background: healthy ? theme.ok : theme.warn,
            boxShadow: `0 0 4px ${healthy ? theme.ok : theme.warn}`,
          }}/>
          {healthy ? 'OPTIMAL' : 'ELEVATED'}
        </div>
      </div>
    </div>
  );
}

// ─── Bitrate / FPS / dropped-frames sparkline block ────────────────────
function StatBlock({ theme, label, value, unit, accent = false, sparkline }) {
  return (
    <div style={{
      padding: '12px 14px',
      background: 'rgba(255,255,255,0.015)',
      border: `1px solid ${theme.chrome.border}`,
      borderRadius: 8,
      display: 'flex', flexDirection: 'column', gap: 6,
      minHeight: 82,
    }}>
      <div style={{
        fontSize: 9.5, letterSpacing: 1.4, color: theme.mute,
        fontFamily: theme.mono, textTransform: 'uppercase', fontWeight: 600,
      }}>{label}</div>
      <div style={{
        display: 'flex', alignItems: 'baseline', gap: 4,
        fontFamily: theme.mono, fontVariantNumeric: 'tabular-nums',
      }}>
        <span style={{
          fontSize: 22, fontWeight: 600, letterSpacing: -0.5,
          color: accent ? 'transparent' : theme.fg,
          ...(accent && {
            background: `linear-gradient(135deg, ${theme.grad.from}, ${theme.grad.to})`,
            WebkitBackgroundClip: 'text', backgroundClip: 'text',
          }),
        }}>{value}</span>
        {unit && <span style={{ fontSize: 10, color: theme.mute, fontWeight: 500, letterSpacing: 0.5 }}>{unit}</span>}
      </div>
      {sparkline}
    </div>
  );
}

// ─── Mini sparkline ────────────────────────────────────────────────────
function Sparkline({ theme, points, width = 140, height = 22 }) {
  const max = Math.max(...points, 1);
  const min = Math.min(...points, 0);
  const span = Math.max(1, max - min);
  const path = points.map((p, i) => {
    const x = (i / (points.length - 1)) * width;
    const y = height - ((p - min) / span) * (height - 2) - 1;
    return `${i === 0 ? 'M' : 'L'}${x.toFixed(1)},${y.toFixed(1)}`;
  }).join(' ');
  const gradId = `spark-${theme.id}-${Math.random().toString(36).slice(2,7)}`;
  return (
    <svg width="100%" height={height} viewBox={`0 0 ${width} ${height}`} preserveAspectRatio="none" style={{ display: 'block' }}>
      <defs>
        <linearGradient id={gradId} x1="0" y1="0" x2="1" y2="0">
          <stop offset="0" stopColor={theme.grad.from} stopOpacity="0.3"/>
          <stop offset="1" stopColor={theme.grad.to}/>
        </linearGradient>
      </defs>
      <path d={path} stroke={`url(#${gradId})`} strokeWidth={1.5} fill="none"/>
    </svg>
  );
}

Object.assign(window, { UrlBar, LatencyHero, StatBlock, Sparkline, fmtTime });
