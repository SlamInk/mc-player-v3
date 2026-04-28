// Direction C — 「零噪声 · 黑白」 Monochrome
// Pure black + bone white. Zero color. The performance IS the aesthetic.
// Every element earns its place; the number is the hero.

const { useState: useStateC, useEffect: useEffectC, useMemo: useMemoC } = React;

const C = {
  bg: '#000000',
  bgSoft: '#0a0a0a',
  panel: 'rgba(255,255,255,.035)',
  border: 'rgba(255,255,255,.08)',
  borderStrong: 'rgba(255,255,255,.16)',
  text: '#ffffff',
  dim: 'rgba(255,255,255,.55)',
  faint: 'rgba(255,255,255,.28)',
  ghost: 'rgba(255,255,255,.12)',
  // Monochrome "gradient" — pure white to mid-grey, used sparingly
  grad: 'linear-gradient(180deg, #ffffff 0%, #8a8a8a 100%)',
  gradH: 'linear-gradient(90deg, #ffffff 0%, #505050 100%)',
  font: '"Inter", "Segoe UI Variable", "Segoe UI", system-ui, sans-serif',
  mono: '"JetBrains Mono", "SF Mono", ui-monospace, Consolas, monospace',
};

function CShell({ children, noise = true, glow = 1, glowOrigin = 'bottom' }) {
  // The signature halo — single white radial glow, shaped like light emission
  const halos = {
    bottom: `
      radial-gradient(ellipse 90% 55% at 50% 105%, rgba(255,255,255,${.22 * glow}), transparent 60%),
      radial-gradient(ellipse 55% 35% at 50% 100%, rgba(255,255,255,${.35 * glow}), transparent 55%),
      radial-gradient(ellipse 30% 20% at 50% 98%, rgba(255,255,255,${.5 * glow}), transparent 50%)
    `,
    center: `
      radial-gradient(ellipse 70% 55% at 50% 50%, rgba(255,255,255,${.18 * glow}), transparent 65%),
      radial-gradient(ellipse 35% 28% at 50% 50%, rgba(255,255,255,${.3 * glow}), transparent 60%)
    `,
    top: `
      radial-gradient(ellipse 90% 50% at 50% -5%, rgba(255,255,255,${.22 * glow}), transparent 60%),
      radial-gradient(ellipse 55% 32% at 50% 0%, rgba(255,255,255,${.4 * glow}), transparent 55%)
    `,
  };
  return (
    <div style={{
      position: 'absolute', inset: 0,
      background: C.bg,
      color: C.text,
      fontFamily: C.font,
      display: 'flex', flexDirection: 'column',
      overflow: 'hidden',
    }}>
      {/* Halo — the performance aura */}
      <div style={{
        position: 'absolute', inset: 0, pointerEvents: 'none',
        background: halos[glowOrigin],
        animation: 'cBreathe 6s ease-in-out infinite',
      }}/>
      {/* Thin light-seam across the halo axis */}
      {glowOrigin === 'bottom' && (
        <div style={{
          position: 'absolute', left: 0, right: 0, bottom: 0, height: 1,
          background: 'linear-gradient(90deg, transparent, rgba(255,255,255,.5), transparent)',
          pointerEvents: 'none',
        }}/>
      )}
      <style>{`@keyframes cBreathe { 0%,100%{opacity:.85} 50%{opacity:1} }`}</style>
      <CornerTicks />
      {noise && <div style={{
        position: 'absolute', inset: 0, pointerEvents: 'none',
        opacity: .025,
        backgroundImage: 'repeating-linear-gradient(0deg, #fff 0 1px, transparent 1px 2px)',
        mixBlendMode: 'overlay',
      }}/>}
      {children}
    </div>
  );
}

function CornerTicks() {
  const tick = (pos) => (
    <div style={{
      position: 'absolute', ...pos, width: 10, height: 10,
      pointerEvents: 'none',
    }}>
      <div style={{ position: 'absolute', ...(pos.top !== undefined ? { top: 0 } : { bottom: 0 }), ...(pos.left !== undefined ? { left: 0 } : { right: 0 }), width: 10, height: 1, background: C.ghost }}/>
      <div style={{ position: 'absolute', ...(pos.top !== undefined ? { top: 0 } : { bottom: 0 }), ...(pos.left !== undefined ? { left: 0 } : { right: 0 }), width: 1, height: 10, background: C.ghost }}/>
    </div>
  );
  return (
    <>
      {tick({ top: 16, left: 16 })}
      {tick({ top: 16, right: 16 })}
      {tick({ bottom: 16, left: 16 })}
      {tick({ bottom: 16, right: 16 })}
    </>
  );
}

function CUrlBar({ value = 'rtsp://192.168.1.64:554/streaming/channels/101', state = 'idle' }) {
  // Dot styling: all monochrome — pure white when live, dim ring when idle,
  // outline-only when in transit, hollow when error
  const dot = () => {
    if (state === 'playing') return (
      <div style={{ position: 'relative', width: 8, height: 8 }}>
        <div style={{ position: 'absolute', inset: 0, borderRadius: 999, background: '#fff' }}/>
        <div style={{ position: 'absolute', inset: -4, borderRadius: 999, background: '#fff', opacity: .18, animation: 'cHalo 1.4s ease-out infinite' }}/>
        <style>{`@keyframes cHalo { 0%{transform:scale(.6);opacity:.35} 100%{transform:scale(2);opacity:0} }`}</style>
      </div>
    );
    if (state === 'connecting') return <div style={{ width: 8, height: 8, borderRadius: 999, border: '1px solid rgba(255,255,255,.5)', animation: 'cRot 1.4s linear infinite' }}><style>{`@keyframes cRot { to { transform: rotate(360deg) } }`}</style></div>;
    if (state === 'error') return <div style={{ width: 8, height: 8, borderRadius: 999, background: '#fff' }}/>;
    return <div style={{ width: 8, height: 8, borderRadius: 999, border: '1px solid rgba(255,255,255,.25)' }}/>;
  };

  return (
    <div style={{
      margin: '68px auto 0', width: 'min(760px, calc(100% - 64px))', position: 'relative', zIndex: 2,
    }}>
      <div style={{
        height: 52,
        background: C.bgSoft,
        border: `1px solid ${state === 'playing' ? C.borderStrong : C.border}`,
        borderRadius: 2,
        display: 'flex', alignItems: 'center',
        padding: '0 6px 0 20px',
        position: 'relative',
        boxShadow: state === 'playing'
          ? '0 0 0 1px rgba(255,255,255,.04), 0 8px 40px rgba(255,255,255,.08), inset 0 1px 0 rgba(255,255,255,.06)'
          : '0 2px 16px rgba(255,255,255,.025), inset 0 1px 0 rgba(255,255,255,.04)',
      }}>
        {/* Gradient underline — the "seam of light" */}
        {state === 'playing' && (
          <div style={{
            position: 'absolute', bottom: -1, left: '8%', right: '8%', height: 1,
            background: 'linear-gradient(90deg, transparent, rgba(255,255,255,.9), transparent)',
            filter: 'blur(.3px)',
          }}/>
        )}
        {dot()}
        <div style={{ width: 1, height: 18, background: C.ghost, margin: '0 16px' }}/>
        <div style={{ fontFamily: C.mono, fontSize: 13, flex: 1, letterSpacing: .2 }}>
          {value
            ? <><span style={{ color: C.faint }}>rtsp://</span><span style={{ color: C.text }}>{value.replace(/^rtsp:\/\//, '')}</span></>
            : <span style={{ color: C.faint }}>paste an rtsp url<span style={{ animation: 'cBlink 1s infinite', marginLeft: 2, color: C.text }}>▌</span></span>}
          <style>{`@keyframes cBlink { 50% { opacity: 0 } }`}</style>
        </div>
        {/* Tiny kbd hint */}
        <div style={{
          fontFamily: C.mono, fontSize: 10, color: C.faint, letterSpacing: 1.5,
          padding: '4px 8px', border: `1px solid ${C.border}`, marginRight: 6,
        }}>⏎</div>
        {/* Play / Pause — pure B&W */}
        <button style={{
          width: 40, height: 40, borderRadius: 2, border: 'none',
          background: state === 'playing' ? 'transparent' : '#fff',
          color: state === 'playing' ? '#fff' : '#000',
          border: state === 'playing' ? `1px solid ${C.borderStrong}` : 'none',
          display: 'flex', alignItems: 'center', justifyContent: 'center',
          cursor: 'default',
          boxShadow: state === 'playing' ? 'none' : '0 0 24px rgba(255,255,255,.25), 0 0 8px rgba(255,255,255,.15)',
        }}>
          {state === 'playing'
            ? <svg width="10" height="12" viewBox="0 0 10 12"><rect x="0" width="3" height="12" fill="currentColor"/><rect x="7" width="3" height="12" fill="currentColor"/></svg>
            : <svg width="10" height="12" viewBox="0 0 10 12"><path d="M0 0 L10 6 L0 12 Z" fill="currentColor"/></svg>}
        </button>
      </div>
    </div>
  );
}

// ─────────── Empty ───────────
function CEmpty() {
  return (
    <CShell glow={1} glowOrigin="bottom">
      <CUrlBar value="" state="idle" />
      <div style={{ flex: 1, display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', position: 'relative' }}>
        {/* Hero wordmark with ghost "80" behind */}
        <div style={{ position: 'relative', display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center' }}>
          <div style={{
            position: 'absolute', top: '50%', left: '50%', transform: 'translate(-50%,-50%)',
            fontFamily: C.mono, fontSize: 280, fontWeight: 100, letterSpacing: -12,
            color: C.ghost, lineHeight: 1, zIndex: 0, pointerEvents: 'none',
          }}>80</div>
          <div style={{
            fontSize: 72, fontWeight: 200, letterSpacing: -3.5, lineHeight: 1,
            zIndex: 1, position: 'relative',
            background: 'linear-gradient(180deg, #ffffff 0%, #ffffff 40%, rgba(255,255,255,.55) 100%)',
            WebkitBackgroundClip: 'text', WebkitTextFillColor: 'transparent',
            filter: 'drop-shadow(0 0 30px rgba(255,255,255,.25))',
          }}>
            mc<span style={{ color: 'rgba(255,255,255,.35)', WebkitTextFillColor: 'rgba(255,255,255,.35)' }}>·</span>player
          </div>
          <div style={{ fontSize: 12, color: C.dim, letterSpacing: 6, fontWeight: 400, marginTop: 14, textTransform: 'uppercase', zIndex: 1, position: 'relative' }}>
            built for 80–130 milliseconds
          </div>
        </div>

        {/* Bottom spec rail */}
        <div style={{
          position: 'absolute', bottom: 40, left: 40, right: 40,
          display: 'flex', justifyContent: 'space-between', alignItems: 'center',
          paddingTop: 14, borderTop: `1px solid ${C.border}`,
          fontFamily: C.mono, fontSize: 10, color: C.faint, letterSpacing: 2, textTransform: 'uppercase',
        }}>
          <span><span style={{ color: C.text }}>TCP</span> · no buffer</span>
          <span><span style={{ color: C.text }}>DISCARD</span> loop filter</span>
          <span><span style={{ color: C.text }}>D3D11VA</span> hwaccel</span>
          <span><span style={{ color: C.text }}>FFMPEG</span> 7.x</span>
          <span><span style={{ color: C.text }}>WIN</span> 10 / 11 x64</span>
        </div>
      </div>
    </CShell>
  );
}

// ─────────── Connecting ───────────
function CConnecting_inner() { return null; }
function CConnecting() {
  const [elapsed, setElapsed] = useStateC(0);
  useEffectC(() => {
    const t0 = Date.now();
    const id = setInterval(() => setElapsed(Math.min(180, Date.now() - t0)), 40);
    return () => clearInterval(id);
  }, []);
  const steps = [
    { t: 12,  label: 'dns · resolved',       done: elapsed > 12 },
    { t: 40,  label: 'tcp · handshake',      done: elapsed > 40 },
    { t: 86,  label: 'rtsp · describe',      done: elapsed > 86 },
    { t: 130, label: 'rtsp · setup / play',  done: elapsed > 130 },
    { t: 180, label: 'probe codec',          done: elapsed > 180 },
  ];

  return (
    <CShell glow={1.1} glowOrigin="center">
      <CUrlBar state="connecting" />
      <div style={{ flex: 1, display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 40, position: 'relative' }}>
        {/* Hero elapsed counter */}
        <div style={{ textAlign: 'center', position: 'relative' }}>
          <div style={{ fontSize: 9, color: C.faint, letterSpacing: 4, textTransform: 'uppercase', fontFamily: C.mono, marginBottom: 12 }}>handshaking</div>
          <div style={{
            fontFamily: C.mono, fontSize: 96, fontWeight: 100, letterSpacing: -5, lineHeight: 1, fontVariantNumeric: 'tabular-nums',
            background: 'linear-gradient(180deg, #ffffff, rgba(255,255,255,.5))',
            WebkitBackgroundClip: 'text', WebkitTextFillColor: 'transparent',
            filter: 'drop-shadow(0 0 40px rgba(255,255,255,.35))',
          }}>
            {String(Math.min(999, elapsed)).padStart(3, '0')}
            <span style={{ fontSize: 22, color: C.faint, marginLeft: 6, letterSpacing: 2, WebkitTextFillColor: 'rgba(255,255,255,.35)' }}>ms</span>
          </div>
        </div>

        {/* Progress hairline with glow */}
        <div style={{ width: 440, height: 1, background: C.border, position: 'relative', overflow: 'visible', boxShadow: '0 0 20px rgba(255,255,255,.15)' }}>
          <div style={{
            position: 'absolute', top: 0, left: 0, height: '100%',
            width: `${Math.min(100, (elapsed / 180) * 100)}%`,
            background: 'linear-gradient(90deg, rgba(255,255,255,.4), #fff)',
            transition: 'width .1s linear',
            boxShadow: '0 0 12px rgba(255,255,255,.8), 0 0 4px rgba(255,255,255,.9)',
          }}/>
          <div style={{
            position: 'absolute', top: -10, left: 0, height: 20, width: 60,
            background: 'radial-gradient(ellipse at center, rgba(255,255,255,.4), transparent 70%)',
            animation: 'cScan 1.4s ease-in-out infinite', pointerEvents: 'none',
          }}/>
          <style>{`@keyframes cScan { 0%{left:-15%} 100%{left:100%} }`}</style>
        </div>

        {/* Step log */}
        <div style={{ fontFamily: C.mono, fontSize: 11, width: 440 }}>
          {steps.map((s, i) => (
            <div key={i} style={{
              display: 'flex', gap: 14, padding: '4px 0',
              color: s.done ? C.dim : C.faint,
            }}>
              <span style={{ width: 40, color: C.faint, fontVariantNumeric: 'tabular-nums' }}>{String(s.t).padStart(3, '0')}</span>
              <span style={{ width: 10, color: s.done ? C.text : C.faint }}>{s.done ? '✓' : '·'}</span>
              <span style={{ color: s.done ? C.text : C.faint }}>{s.label}</span>
            </div>
          ))}
        </div>
      </div>
    </CShell>
  );
}

// ─────────── Playing ───────────
function FakeSignalMono({ dim = false }) {
  return (
    <div style={{
      position: 'absolute', inset: 0,
      background: `
        radial-gradient(ellipse at 30% 40%, rgba(255,255,255,.10), transparent 55%),
        radial-gradient(ellipse at 70% 60%, rgba(255,255,255,.07), transparent 55%),
        #050505
      `,
      overflow: 'hidden',
      opacity: dim ? .55 : 1,
    }}>
      <div style={{
        position: 'absolute', left: '50%', top: '58%', transform: 'translate(-50%,-50%)',
        width: '42%', height: '58%',
        background: 'radial-gradient(ellipse at 50% 30%, rgba(255,255,255,.18), transparent 68%)',
        filter: 'blur(5px)',
      }}/>
      <div style={{
        position: 'absolute', inset: 0,
        backgroundImage: 'repeating-linear-gradient(0deg, rgba(255,255,255,.02) 0 1px, transparent 1px 3px)',
        mixBlendMode: 'overlay',
      }}/>
    </div>
  );
}

function CPlaying({ showStats = false }) {
  const latency = useLiveLatency(92, 14);
  return (
    <CShell noise={false} glow={0.35} glowOrigin="bottom">
      <CUrlBar state="playing" />
      <div style={{
        flex: 1, margin: '20px 40px 40px', position: 'relative',
        border: `1px solid ${C.border}`, overflow: 'hidden',
        boxShadow: '0 24px 60px rgba(255,255,255,.05), 0 0 0 1px rgba(255,255,255,.03), inset 0 1px 0 rgba(255,255,255,.06)',
      }}>
        <FakeSignalMono />
        {/* Signature glow along bottom edge of video frame */}
        <div style={{
          position: 'absolute', left: 0, right: 0, bottom: 0, height: 2,
          background: 'linear-gradient(90deg, transparent 10%, rgba(255,255,255,.8) 50%, transparent 90%)',
          boxShadow: '0 0 16px rgba(255,255,255,.35)',
          pointerEvents: 'none',
        }}/>

        {/* TOP-LEFT: framing marks */}
        <div style={{ position: 'absolute', top: 10, left: 10, width: 18, height: 18 }}>
          <div style={{ position: 'absolute', top: 0, left: 0, width: 14, height: 1, background: '#fff', opacity: .7 }}/>
          <div style={{ position: 'absolute', top: 0, left: 0, width: 1, height: 14, background: '#fff', opacity: .7 }}/>
        </div>
        <div style={{ position: 'absolute', top: 10, right: 10, width: 18, height: 18 }}>
          <div style={{ position: 'absolute', top: 0, right: 0, width: 14, height: 1, background: '#fff', opacity: .7 }}/>
          <div style={{ position: 'absolute', top: 0, right: 0, width: 1, height: 14, background: '#fff', opacity: .7 }}/>
        </div>
        <div style={{ position: 'absolute', bottom: 10, left: 10, width: 18, height: 18 }}>
          <div style={{ position: 'absolute', bottom: 0, left: 0, width: 14, height: 1, background: '#fff', opacity: .7 }}/>
          <div style={{ position: 'absolute', bottom: 0, left: 0, width: 1, height: 14, background: '#fff', opacity: .7 }}/>
        </div>
        <div style={{ position: 'absolute', bottom: 10, right: 10, width: 18, height: 18 }}>
          <div style={{ position: 'absolute', bottom: 0, right: 0, width: 14, height: 1, background: '#fff', opacity: .7 }}/>
          <div style={{ position: 'absolute', bottom: 0, right: 0, width: 1, height: 14, background: '#fff', opacity: .7 }}/>
        </div>

        {/* TOP-LEFT HUD — large tabular latency with glow */}
        <div style={{
          position: 'absolute', top: 28, left: 28,
          display: 'flex', alignItems: 'baseline', gap: 10,
        }}>
          <div style={{
            fontFamily: C.mono, fontSize: 34, fontWeight: 200, letterSpacing: -1.5,
            fontVariantNumeric: 'tabular-nums', lineHeight: 1,
            background: 'linear-gradient(180deg, #ffffff, rgba(255,255,255,.65))',
            WebkitBackgroundClip: 'text', WebkitTextFillColor: 'transparent',
            filter: 'drop-shadow(0 0 12px rgba(255,255,255,.5))',
          }}>
            {String(latency).padStart(3, '0')}
          </div>
          <div style={{ fontFamily: C.mono, fontSize: 10, color: C.faint, letterSpacing: 3, textTransform: 'uppercase' }}>ms · e2e</div>
        </div>

        {/* TOP-RIGHT — LIVE + frame ruler */}
        <div style={{
          position: 'absolute', top: 28, right: 28,
          display: 'flex', flexDirection: 'column', alignItems: 'flex-end', gap: 10,
        }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 8, fontFamily: C.mono, fontSize: 10, letterSpacing: 3, textTransform: 'uppercase' }}>
            <div style={{
              width: 6, height: 6, borderRadius: 999, background: '#fff',
              boxShadow: '0 0 10px rgba(255,255,255,.8), 0 0 3px #fff',
              animation: 'cLive 1s infinite',
            }}/>
            <style>{`@keyframes cLive { 50% { opacity: .3 } }`}</style>
            <span>LIVE</span>
          </div>
          <div style={{ fontFamily: C.mono, fontSize: 10, color: C.faint, letterSpacing: 2 }}>
            1920 × 1080 · 25.00 fps
          </div>
        </div>

        {/* BOTTOM-LEFT stat trio */}
        <div style={{
          position: 'absolute', bottom: 28, left: 28,
          display: 'flex', gap: 22,
          fontFamily: C.mono, fontSize: 11, color: C.dim,
        }}>
          <span><span style={{ color: C.faint }}>cdc</span> h.264</span>
          <span><span style={{ color: C.faint }}>br</span> 3.2m</span>
          <span><span style={{ color: C.faint }}>hw</span> d3d11va</span>
          <span><span style={{ color: C.faint }}>drp</span> 0</span>
        </div>

        {/* BOTTOM-RIGHT inline sparkline */}
        <CInlineSpark />

        {showStats && <CStatsDrawer latency={latency} />}
      </div>
    </CShell>
  );
}

function CInlineSpark() {
  const pts = useMemoC(() => Array.from({ length: 42 }, (_, i) => 50 + Math.sin(i * .38) * 10 + Math.random() * 6), []);
  const path = pts.map((v, i) => `${i === 0 ? 'M' : 'L'}${(i / (pts.length - 1)) * 140},${22 - (v / 100) * 18}`).join(' ');
  return (
    <div style={{
      position: 'absolute', bottom: 24, right: 28,
      display: 'flex', alignItems: 'center', gap: 10,
    }}>
      <span style={{ fontFamily: C.mono, fontSize: 9, color: C.faint, letterSpacing: 2, textTransform: 'uppercase' }}>p99 118</span>
      <svg width="140" height="22" style={{ display: 'block' }}>
        <path d={path} stroke="rgba(255,255,255,.75)" fill="none" strokeWidth="1"/>
        <line x1="0" y1="6" x2="140" y2="6" stroke="rgba(255,255,255,.12)" strokeDasharray="2 3"/>
      </svg>
    </div>
  );
}

function CStatsDrawerUnused() { return null; }
function CStatsDrawer({ latency }) {
  const spark = useMemoC(() => Array.from({ length: 60 }, (_, i) => 48 + Math.sin(i * .25) * 8 + Math.random() * 6), []);
  const path = spark.map((v, i) => `${i === 0 ? 'M' : 'L'}${(i / (spark.length - 1)) * 100},${30 - (v / 100) * 26}`).join(' ');

  const row = (k, v, unit) => (
    <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: 10.5, padding: '5px 0', color: C.dim, borderBottom: `1px solid ${C.border}` }}>
      <span style={{ letterSpacing: 1.5, textTransform: 'uppercase', color: C.faint }}>{k}</span>
      <span style={{ color: C.text, fontVariantNumeric: 'tabular-nums' }}>{v}{unit && <span style={{ color: C.faint, marginLeft: 2 }}>{unit}</span>}</span>
    </div>
  );

  return (
    <div style={{
      position: 'absolute', top: 20, right: 20,
      width: 240,
      background: 'rgba(0,0,0,.78)',
      backdropFilter: 'blur(20px) saturate(120%)',
      WebkitBackdropFilter: 'blur(20px) saturate(120%)',
      border: `1px solid ${C.borderStrong}`,
      padding: 18,
      fontFamily: C.mono,
    }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', marginBottom: 2 }}>
        <span style={{ fontSize: 9, letterSpacing: 2.5, color: C.faint, textTransform: 'uppercase' }}>Latency</span>
        <span style={{ fontSize: 9, color: C.faint, letterSpacing: 1 }}>avg 94 · p99 118</span>
      </div>
      <div style={{
        fontSize: 44, fontWeight: 100, letterSpacing: -2, fontVariantNumeric: 'tabular-nums', lineHeight: 1.1, marginTop: 4,
        background: 'linear-gradient(180deg, #ffffff, rgba(255,255,255,.55))',
        WebkitBackgroundClip: 'text', WebkitTextFillColor: 'transparent',
        filter: 'drop-shadow(0 0 18px rgba(255,255,255,.4))',
      }}>
        {latency}<span style={{ fontSize: 13, color: C.dim, marginLeft: 3, letterSpacing: 1, WebkitTextFillColor: 'rgba(255,255,255,.45)' }}>ms</span>
      </div>
      <svg width="100%" height="32" viewBox="0 0 100 32" preserveAspectRatio="none" style={{ marginTop: 4, display: 'block' }}>
        <path d={path} stroke="#fff" fill="none" strokeWidth=".7" vectorEffect="non-scaling-stroke"/>
        <line x1="0" y1="6" x2="100" y2="6" stroke="rgba(255,255,255,.15)" strokeDasharray="1 2" strokeWidth=".3" vectorEffect="non-scaling-stroke"/>
      </svg>
      <div style={{ height: 1, background: C.border, margin: '12px -18px' }}/>
      {row('fps', '25.00')}
      {row('res', '1920 × 1080')}
      {row('codec', 'h.264 main')}
      {row('hwaccel', 'd3d11va')}
      {row('bitrate', '3.2', 'mbps')}
      {row('transport', 'tcp')}
      {row('dropped', '0')}
    </div>
  );
}

// ─────────── Error ───────────
function CError() {
  return (
    <CShell glow={0.4} glowOrigin="center">
      <CUrlBar state="error" />
      <div style={{ flex: 1, display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 22, position: 'relative' }}>
        {/* Big X in mono */}
        <div style={{ position: 'relative', width: 80, height: 80 }}>
          <div style={{ position: 'absolute', inset: 0, border: `1px solid ${C.borderStrong}`, borderRadius: 999 }}/>
          <div style={{ position: 'absolute', inset: 0, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
            <svg width="30" height="30" viewBox="0 0 30 30"><line x1="6" y1="6" x2="24" y2="24" stroke="#fff" strokeWidth="1.2"/><line x1="24" y1="6" x2="6" y2="24" stroke="#fff" strokeWidth="1.2"/></svg>
          </div>
        </div>
        <div style={{ textAlign: 'center' }}>
          <div style={{ fontSize: 15, letterSpacing: .5, marginBottom: 10, fontWeight: 400 }}>
            <span style={{ fontFamily: C.mono, color: C.text }}>CONNECTION_TIMEOUT</span>
          </div>
          <div style={{ fontFamily: C.mono, fontSize: 11, color: C.dim, lineHeight: 1.85 }}>
            no SDP received within 1500ms<br/>
            <span style={{ color: C.faint }}>// rtsp://192.168.1.64:554/streaming/channels/101</span>
          </div>
        </div>
        {/* Diagnostic triplet */}
        <div style={{ display: 'flex', gap: 0, fontFamily: C.mono, fontSize: 10, marginTop: 2 }}>
          {[
            ['host', 'reachable', true],
            ['port', '554 open', true],
            ['auth', 'unverified', false],
          ].map(([k, v, ok], i) => (
            <div key={i} style={{ padding: '8px 14px', border: `1px solid ${C.border}`, borderLeft: i === 0 ? `1px solid ${C.border}` : 'none', color: C.dim }}>
              <span style={{ color: ok ? C.text : C.faint, marginRight: 6 }}>{ok ? '✓' : '✕'}</span>
              <span style={{ color: C.faint, marginRight: 4 }}>{k}</span>
              <span style={{ color: ok ? C.text : C.dim }}>{v}</span>
            </div>
          ))}
        </div>
        <div style={{ display: 'flex', gap: 6, marginTop: 6 }}>
          <button style={{
            padding: '10px 22px', borderRadius: 2, border: 'none',
            background: '#fff', color: '#000', fontSize: 11, fontWeight: 600,
            fontFamily: C.mono, letterSpacing: 2.5, textTransform: 'uppercase', cursor: 'default',
          }}>RETRY</button>
          <button style={{
            padding: '10px 22px', borderRadius: 2,
            background: 'transparent', color: C.dim, fontSize: 11, fontFamily: C.mono,
            border: `1px solid ${C.border}`, letterSpacing: 2.5, textTransform: 'uppercase', cursor: 'default',
          }}>DIAGNOSTICS</button>
        </div>
      </div>
    </CShell>
  );
}

// ─────────── Add / Edit Stream Modal ───────────
function CAddModal() {
  return (
    <CShell glow={0.5} glowOrigin="bottom">
      <CUrlBar state="idle" />
      <div style={{ position: 'absolute', inset: 0, background: 'rgba(0,0,0,.65)', backdropFilter: 'blur(6px)', zIndex: 5 }}/>
      <div style={{
        position: 'absolute', top: '50%', left: '50%', transform: 'translate(-50%,-50%)',
        width: 480, zIndex: 10,
        background: '#000',
        border: `1px solid ${C.borderStrong}`,
        padding: 28,
        boxShadow: '0 40px 100px rgba(0,0,0,.9), 0 0 60px rgba(255,255,255,.08), 0 0 0 1px rgba(255,255,255,.05)',
      }}>
        {/* thin top accent */}
        <div style={{ position: 'absolute', top: 0, left: 0, right: 0, height: 1, background: 'linear-gradient(90deg, transparent, #fff, transparent)' }}/>

        <div style={{ fontSize: 10, letterSpacing: 3, color: C.faint, textTransform: 'uppercase', fontFamily: C.mono, marginBottom: 6 }}>// new source</div>
        <div style={{ fontSize: 18, fontWeight: 300, marginBottom: 24, letterSpacing: -.3 }}>添加 RTSP 流</div>

        <div style={{ fontSize: 9, color: C.faint, letterSpacing: 2, textTransform: 'uppercase', marginBottom: 6, fontFamily: C.mono }}>Name</div>
        <div style={{
          height: 38, padding: '0 12px', marginBottom: 18,
          background: C.bgSoft, border: `1px solid ${C.border}`,
          display: 'flex', alignItems: 'center',
          fontSize: 13, color: C.text,
        }}>前门摄像头 · 101</div>

        <div style={{ fontSize: 9, color: C.faint, letterSpacing: 2, textTransform: 'uppercase', marginBottom: 6, fontFamily: C.mono }}>RTSP URL</div>
        <div style={{
          height: 38, padding: '0 12px', marginBottom: 8,
          background: C.bgSoft, border: `1px solid ${C.borderStrong}`,
          display: 'flex', alignItems: 'center',
          fontFamily: C.mono, fontSize: 12, color: C.text,
        }}>
          rtsp://admin:xxx@192.168.1.64:554/streaming/channels/101
          <span style={{ display: 'inline-block', width: 1, height: 14, background: '#fff', marginLeft: 2, animation: 'cCaret 1s infinite' }}/>
        </div>
        <style>{`@keyframes cCaret { 50% { opacity: 0 } }`}</style>

        {/* Preflight check */}
        <div style={{
          padding: 12, marginBottom: 22,
          border: `1px solid ${C.border}`,
          fontFamily: C.mono, fontSize: 10.5, color: C.dim, lineHeight: 1.8,
        }}>
          <div><span style={{ color: C.text }}>✓</span> syntax ok</div>
          <div><span style={{ color: C.text }}>✓</span> host reachable · ping 2ms</div>
          <div><span style={{ color: C.faint }}>·</span> transport = <span style={{ color: C.text }}>tcp</span> · auth = <span style={{ color: C.text }}>basic</span></div>
        </div>

        <div style={{ display: 'flex', justifyContent: 'flex-end', gap: 6 }}>
          <button style={{
            padding: '9px 18px',
            background: 'transparent', color: C.dim, fontSize: 11,
            border: `1px solid ${C.border}`, fontFamily: C.mono, letterSpacing: 2, textTransform: 'uppercase',
            cursor: 'default',
          }}>取消</button>
          <button style={{
            padding: '9px 22px', border: 'none',
            background: '#fff', color: '#000', fontSize: 11, fontWeight: 600,
            fontFamily: C.mono, letterSpacing: 2, textTransform: 'uppercase',
            cursor: 'default',
          }}>保存并播放</button>
        </div>
      </div>
    </CShell>
  );
}

Object.assign(window, { CEmpty, CConnecting, CPlaying, CError, CAddModal });
