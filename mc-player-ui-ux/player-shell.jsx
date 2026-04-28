// MC Player — shared shell: Windows 11 chrome, icons, small primitives.
// Each theme (V1/V2/V3) supplies its own gradient + accent tokens.

const { useState, useEffect, useRef, useMemo } = React;

// ─── Icons ─────────────────────────────────────────────────────────────
const Icon = ({ d, size = 14, stroke = 1.6 }) => (
  <svg width={size} height={size} viewBox="0 0 24 24" fill="none"
       stroke="currentColor" strokeWidth={stroke} strokeLinecap="round" strokeLinejoin="round">
    {typeof d === 'string' ? <path d={d}/> : d}
  </svg>
);
const I = {
  play:    'M8 5v14l11-7z',
  stop:    <rect x="6" y="6" width="12" height="12" rx="1.5"/>,
  power:   <><path d="M12 3v9"/><path d="M5.6 7.6a8 8 0 1 0 12.8 0"/></>,
  paste:   <><rect x="8" y="4" width="12" height="16" rx="2"/><path d="M4 8v12a2 2 0 0 0 2 2h10"/></>,
  gear:    <><circle cx="12" cy="12" r="3.2"/><path d="M12 2v3 M12 19v3 M4.2 4.2l2.1 2.1 M17.7 17.7l2.1 2.1 M2 12h3 M19 12h3 M4.2 19.8l2.1-2.1 M17.7 6.3l2.1-2.1"/></>,
  info:    <><circle cx="12" cy="12" r="9"/><path d="M12 16v-5 M12 8h.01"/></>,
  warn:    <><path d="M10.3 3.9 2.2 17.9A2 2 0 0 0 3.9 21h16.2a2 2 0 0 0 1.7-3.1L13.7 3.9a2 2 0 0 0-3.4 0z"/><path d="M12 9v4 M12 17h.01"/></>,
  retry:   <><path d="M21 12a9 9 0 1 1-3-6.7"/><path d="M21 4v5h-5"/></>,
  copy:    <><rect x="9" y="9" width="11" height="11" rx="1.5"/><path d="M5 15H4a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1h10a1 1 0 0 1 1 1v1"/></>,
  link:    <><path d="M10 13a5 5 0 0 0 7 0l3-3a5 5 0 0 0-7-7l-1 1"/><path d="M14 11a5 5 0 0 0-7 0l-3 3a5 5 0 0 0 7 7l1-1"/></>,
  close:   'M6 6l12 12 M18 6l-12 12',
  maxIcn:  <rect x="4" y="4" width="16" height="16" rx="1.5"/>,
  minIcn:  'M5 12h14',
  chev:    'M9 18l6-6-6-6',
  bolt:    'M13 2 4 14h7l-1 8 9-12h-7z',
  x:       'M6 6l12 12 M18 6l-12 12',
  vol:     <><polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/><path d="M15 9a4 4 0 0 1 0 6"/></>,
  muted:   <><polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/><path d="M22 9l-6 6 M16 9l6 6"/></>,
  dot:     <circle cx="12" cy="12" r="4"/>,
  snap:    <><rect x="3" y="6" width="18" height="13" rx="2"/><circle cx="12" cy="12.5" r="3.5"/><path d="M8 6l2-2h4l2 2"/></>,
  exp:     'M4 10V4h6 M20 14v6h-6 M4 4l6 6 M20 20l-6-6',
};

// ─── Win11 frame ───────────────────────────────────────────────────────
function Win11Frame({ theme, children, width, height, titleRight }) {
  const titlebarH = 32;
  return (
    <div style={{
      width, height, display: 'flex', flexDirection: 'column',
      background: theme.bg, color: theme.fg, fontFamily: theme.ui,
      borderRadius: 10, overflow: 'hidden',
      boxShadow: '0 30px 80px rgba(0,0,0,0.45), 0 4px 12px rgba(0,0,0,0.3)',
      border: `1px solid ${theme.chrome.border}`,
      position: 'relative',
    }}>
      {/* Titlebar */}
      <div style={{
        height: titlebarH, flexShrink: 0, display: 'flex', alignItems: 'center',
        paddingLeft: 12, background: theme.chrome.titleBg,
        borderBottom: `1px solid ${theme.chrome.border}`, userSelect: 'none',
        position: 'relative', zIndex: 3,
      }}>
        <MCLogoMark theme={theme} size={14}/>
        <div style={{
          marginLeft: 10, fontSize: 11.5, color: theme.fgDim, letterSpacing: 0.2,
          fontFamily: theme.ui, flex: 1,
          display: 'flex', alignItems: 'center', gap: 10,
        }}>
          <span style={{ color: theme.fg, fontWeight: 500 }}>MC Player</span>
          <span style={{ color: theme.mute }}>·</span>
          <span style={{ color: theme.mute, fontFamily: theme.mono, fontSize: 10.5 }}>ultra-low-latency rtsp</span>
          <span style={{ flex: 1 }}/>
          {titleRight}
        </div>
        <div style={{ display: 'flex', height: titlebarH }}>
          <CaptionBtn theme={theme}><svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.4"><path d="M5 12h14"/></svg></CaptionBtn>
          <CaptionBtn theme={theme}><svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.4"><rect x="4" y="4" width="16" height="16" rx="1"/></svg></CaptionBtn>
          <CaptionBtn theme={theme} hoverBg="#c42b1c" hoverFg="#fff"><svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.4"><path d="M6 6l12 12 M18 6l-12 12"/></svg></CaptionBtn>
        </div>
      </div>

      {/* Body */}
      <div style={{ flex: 1, minHeight: 0, position: 'relative' }}>
        {children}
      </div>
    </div>
  );
}

function CaptionBtn({ theme, hoverBg, hoverFg, children }) {
  const [h, setH] = useState(false);
  return (
    <button
      onMouseEnter={() => setH(true)} onMouseLeave={() => setH(false)}
      style={{
        width: 46, height: 32, border: 'none', cursor: 'default',
        background: h ? (hoverBg || 'rgba(255,255,255,0.06)') : 'transparent',
        color: h ? (hoverFg || theme.fg) : theme.fgDim,
        display: 'flex', alignItems: 'center', justifyContent: 'center',
      }}>{children}</button>
  );
}

// ─── Logo ──────────────────────────────────────────────────────────────
function MCLogoMark({ theme, size = 14 }) {
  const gradId = 'mc-logo-' + theme.id;
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" style={{ flexShrink: 0 }}>
      <defs>
        <linearGradient id={gradId} x1="0" y1="0" x2="1" y2="1">
          <stop offset="0" stopColor={theme.grad.from}/>
          <stop offset="1" stopColor={theme.grad.to}/>
        </linearGradient>
      </defs>
      <rect x="2" y="2" width="20" height="20" rx="5" fill={`url(#${gradId})`}/>
      <path d="M7 16V9l3 4 3-4v7 M16 16V9h1.5a2.5 2.5 0 0 1 0 5H16"
            stroke={theme.chrome.titleBg} strokeWidth="1.5" fill="none" strokeLinecap="round" strokeLinejoin="round"/>
    </svg>
  );
}

// ─── Gradient text ─────────────────────────────────────────────────────
function GradText({ theme, children, style = {} }) {
  return (
    <span style={{
      background: `linear-gradient(135deg, ${theme.grad.from}, ${theme.grad.mid || theme.grad.to}, ${theme.grad.to})`,
      WebkitBackgroundClip: 'text', backgroundClip: 'text',
      WebkitTextFillColor: 'transparent', color: 'transparent',
      ...style,
    }}>{children}</span>
  );
}

// ─── Atmospheric background — radial gradients sitting behind the body
function Atmosphere({ theme, variant = 'playing' }) {
  // Keep it subtle — gradients present but content must stay readable.
  const intensity = { empty: 0.35, connecting: 0.5, playing: 0.28, error: 0.22 }[variant] || 0.3;
  return (
    <div aria-hidden style={{
      position: 'absolute', inset: 0, overflow: 'hidden', pointerEvents: 'none', zIndex: 0,
    }}>
      <div style={{
        position: 'absolute', left: '-10%', top: '-20%',
        width: '70%', height: '70%',
        background: `radial-gradient(circle at 30% 30%, ${theme.grad.from}${Math.round(intensity*255).toString(16).padStart(2,'0')} 0%, transparent 60%)`,
        filter: 'blur(40px)',
      }}/>
      <div style={{
        position: 'absolute', right: '-15%', bottom: '-25%',
        width: '75%', height: '75%',
        background: `radial-gradient(circle at 70% 70%, ${theme.grad.to}${Math.round(intensity*255).toString(16).padStart(2,'0')} 0%, transparent 60%)`,
        filter: 'blur(50px)',
      }}/>
      {/* Grain-ish overlay via layered tiny noise — use SVG for crispness */}
      <svg style={{ position: 'absolute', inset: 0, width: '100%', height: '100%', opacity: 0.04, mixBlendMode: 'overlay' }}>
        <filter id={`noise-${theme.id}`}>
          <feTurbulence baseFrequency="0.9" numOctaves="2" stitchTiles="stitch"/>
        </filter>
        <rect width="100%" height="100%" filter={`url(#noise-${theme.id})`}/>
      </svg>
    </div>
  );
}

Object.assign(window, { Icon, I, Win11Frame, CaptionBtn, MCLogoMark, GradText, Atmosphere });
