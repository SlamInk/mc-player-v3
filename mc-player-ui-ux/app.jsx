// MC Player — Windows 超低延时播放器 UI/UX
// 3 directions × multiple states, on a design canvas.

const { useState, useEffect, useRef, useMemo } = React;

// ─────────────────────────────────────────────────────────────
// Windows 11 window chrome
// ─────────────────────────────────────────────────────────────
function Win11Frame({ title = 'MC Player', children, accent = '#ffffff', onTitle, width = 1120, height = 720 }) {
  return (
    <div style={{
      width, height,
      borderRadius: 8,
      overflow: 'hidden',
      boxShadow: '0 24px 80px rgba(0,0,0,.45), 0 4px 16px rgba(0,0,0,.3), 0 0 0 1px rgba(255,255,255,.04)',
      background: '#000',
      display: 'flex',
      flexDirection: 'column',
      position: 'relative',
      fontFamily: '"Segoe UI Variable", "Segoe UI", system-ui, sans-serif',
    }}>
      <div style={{
        height: 36,
        display: 'flex',
        alignItems: 'center',
        paddingLeft: 14,
        color: 'rgba(255,255,255,.72)',
        fontSize: 12,
        letterSpacing: .2,
        flexShrink: 0,
        background: 'transparent',
        position: 'absolute',
        top: 0, left: 0, right: 0,
        zIndex: 20,
        WebkitAppRegion: 'drag',
      }}>
        <span style={{ fontWeight: 600, color: accent, marginRight: 6, opacity: .9 }}>◆</span>
        <span>{title}</span>
        <div style={{ flex: 1 }} />
        <WinControls />
      </div>
      <div style={{ flex: 1, position: 'relative', overflow: 'hidden' }}>
        {children}
      </div>
    </div>
  );
}

function WinControls() {
  const btn = {
    width: 46, height: 36,
    display: 'inline-flex', alignItems: 'center', justifyContent: 'center',
    color: 'rgba(255,255,255,.7)',
    transition: 'background .12s',
    cursor: 'default',
  };
  return (
    <div style={{ display: 'flex', alignItems: 'center', height: 36 }}>
      <div style={btn}><svg width="10" height="10" viewBox="0 0 10 10"><line x1="0" y1="5" x2="10" y2="5" stroke="currentColor" strokeWidth="1"/></svg></div>
      <div style={btn}><svg width="10" height="10" viewBox="0 0 10 10"><rect x=".5" y=".5" width="9" height="9" stroke="currentColor" strokeWidth="1" fill="none"/></svg></div>
      <div style={{...btn, width: 46}}><svg width="10" height="10" viewBox="0 0 10 10"><line x1="0" y1="0" x2="10" y2="10" stroke="currentColor" strokeWidth="1"/><line x1="10" y1="0" x2="0" y2="10" stroke="currentColor" strokeWidth="1"/></svg></div>
    </div>
  );
}

// ─────────────────────────────────────────────────────────────
// Simulated video "signal" — a subtly moving gradient with scan lines
// ─────────────────────────────────────────────────────────────
function FakeSignal({ hue = 180, dim = false }) {
  return (
    <div style={{
      position: 'absolute', inset: 0,
      background: `
        radial-gradient(ellipse at 30% 40%, hsla(${hue}, 40%, 30%, .8), transparent 60%),
        radial-gradient(ellipse at 70% 60%, hsla(${hue + 40}, 35%, 25%, .7), transparent 55%),
        linear-gradient(180deg, #0a0a0e 0%, #05050a 100%)
      `,
      overflow: 'hidden',
      opacity: dim ? .45 : 1,
    }}>
      {/* Fake subject silhouette */}
      <div style={{
        position: 'absolute', left: '50%', top: '62%',
        transform: 'translate(-50%,-50%)',
        width: '38%', height: '56%',
        background: `radial-gradient(ellipse at 50% 30%, hsla(${hue}, 20%, 60%, .25), hsla(${hue}, 10%, 10%, 0) 70%)`,
        filter: 'blur(4px)',
      }} />
      {/* Scanline grain */}
      <div style={{
        position: 'absolute', inset: 0,
        backgroundImage: 'repeating-linear-gradient(0deg, rgba(255,255,255,.018) 0 1px, transparent 1px 3px)',
        mixBlendMode: 'overlay',
      }} />
    </div>
  );
}

// Shared: animated tick hook
function useTick(ms = 900) {
  const [t, setT] = useState(0);
  useEffect(() => {
    const id = setInterval(() => setT(x => x + 1), ms);
    return () => clearInterval(id);
  }, [ms]);
  return t;
}

// A "live" 3-digit latency value that wiggles
function useLiveLatency(base = 98, jitter = 14) {
  const [v, setV] = useState(base);
  useEffect(() => {
    const id = setInterval(() => {
      setV(Math.max(72, Math.min(142, base + Math.round((Math.random() - .5) * jitter))));
    }, 600);
    return () => clearInterval(id);
  }, [base, jitter]);
  return v;
}

Object.assign(window, { Win11Frame, FakeSignal, useTick, useLiveLatency });
