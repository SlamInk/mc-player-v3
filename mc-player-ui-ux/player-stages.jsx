// MC Player — state-specific stages: Empty, Connecting, Playing, Error.
// Each is the full video area of the player (excludes chrome + URL bar).

const { useState: useStateST, useEffect: useEffectST, useRef: useRefST } = React;

// ─── Empty state — hero invitation ─────────────────────────────────────
function EmptyStage({ theme }) {
  return (
    <div style={{
      flex: 1, position: 'relative', display: 'flex',
      alignItems: 'center', justifyContent: 'center',
      overflow: 'hidden',
    }}>
      <Atmosphere theme={theme} variant="empty"/>
      <div style={{ position: 'relative', zIndex: 1, textAlign: 'center', maxWidth: 560 }}>
        {/* Gradient mark */}
        <div style={{
          width: 72, height: 72, margin: '0 auto 28px', position: 'relative',
          borderRadius: 18,
          background: `linear-gradient(135deg, ${theme.grad.from}, ${theme.grad.to})`,
          display: 'flex', alignItems: 'center', justifyContent: 'center',
          boxShadow: `0 0 0 1px rgba(255,255,255,0.06), 0 20px 50px -10px ${theme.grad.to}66`,
        }}>
          <svg width="34" height="34" viewBox="0 0 24 24" fill="none" stroke={theme.grad.onGrad} strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
            <path d={I.bolt}/>
          </svg>
        </div>

        <div style={{
          fontSize: 32, fontWeight: 700, letterSpacing: -0.8,
          lineHeight: 1.15, marginBottom: 12, color: theme.fg,
        }}>
          <GradText theme={theme}>极速</GradText>
          <span style={{ color: theme.fg }}>之间,</span>
          <br/>
          <span style={{ color: theme.fg }}>只剩 </span>
          <span style={{ fontFamily: theme.mono, fontWeight: 600 }}>
            <GradText theme={theme}>80–130</GradText>
          </span>
          <span style={{ fontFamily: theme.mono, color: theme.fgDim, fontWeight: 500 }}> ms</span>
        </div>

        <div style={{
          fontSize: 13.5, color: theme.fgDim, lineHeight: 1.6, marginBottom: 34,
          maxWidth: 440, marginLeft: 'auto', marginRight: 'auto',
        }}>
          粘贴 RTSP 地址开始播放。TCP 传输 · nobuffer 解复用 · D3D11VA 硬解。
          单流,零缓冲,直达屏幕。
        </div>

        {/* CTA hint */}
        <div style={{
          display: 'inline-flex', alignItems: 'center', gap: 10,
          padding: '10px 16px', borderRadius: 999,
          background: 'rgba(255,255,255,0.03)',
          border: `1px solid ${theme.chrome.border}`,
          fontFamily: theme.mono, fontSize: 11, color: theme.fgDim,
        }}>
          <Icon d={I.paste} size={12}/>
          <span>在顶部粘贴 URL,或按</span>
          <kbd style={{
            fontFamily: theme.mono, fontSize: 10.5, fontWeight: 600,
            padding: '2px 6px', borderRadius: 4,
            background: 'rgba(255,255,255,0.06)', color: theme.fg,
            border: `1px solid ${theme.chrome.border}`,
          }}>Ctrl+V</kbd>
        </div>

        {/* Feature tags */}
        <div style={{
          marginTop: 48, display: 'flex', gap: 28, justifyContent: 'center',
          flexWrap: 'wrap',
        }}>
          {[
            ['TCP',        '抗丢包重排'],
            ['NOBUFFER',   '零解复用缓冲'],
            ['LOW_DELAY',  '禁用帧重排'],
            ['D3D11VA',    'GPU 硬解'],
          ].map(([k, d]) => (
            <div key={k} style={{ textAlign: 'left' }}>
              <div style={{
                fontFamily: theme.mono, fontSize: 10.5, fontWeight: 700,
                letterSpacing: 1.2,
              }}>
                <GradText theme={theme}>{k}</GradText>
              </div>
              <div style={{ fontSize: 10.5, color: theme.mute, marginTop: 2 }}>{d}</div>
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}

// ─── Connecting state — protocol handshake visualization ───────────────
function ConnectingStage({ theme, url }) {
  const steps = [
    { id: 'dns',      label: 'DNS',            done: true  },
    { id: 'tcp',      label: 'TCP · 554',      done: true  },
    { id: 'describe', label: 'DESCRIBE · SDP', done: true  },
    { id: 'setup',    label: 'SETUP',          done: false, active: true },
    { id: 'play',     label: 'PLAY',           done: false },
    { id: 'decode',   label: 'DECODE · H264',  done: false },
  ];
  return (
    <div style={{
      flex: 1, position: 'relative', display: 'flex',
      alignItems: 'center', justifyContent: 'center', overflow: 'hidden',
    }}>
      <Atmosphere theme={theme} variant="connecting"/>
      <div style={{ position: 'relative', zIndex: 1, width: 560, maxWidth: '90%' }}>
        {/* Spinning ring */}
        <div style={{ position: 'relative', width: 120, height: 120, margin: '0 auto 32px' }}>
          <svg width="120" height="120" viewBox="0 0 120 120" style={{ animation: 'mc-spin 1.2s linear infinite' }}>
            <defs>
              <linearGradient id={`conn-${theme.id}`} x1="0" y1="0" x2="1" y2="1">
                <stop offset="0" stopColor={theme.grad.from} stopOpacity="0"/>
                <stop offset="0.5" stopColor={theme.grad.mid || theme.grad.to}/>
                <stop offset="1" stopColor={theme.grad.to}/>
              </linearGradient>
            </defs>
            <circle cx="60" cy="60" r="50" fill="none" stroke="rgba(255,255,255,0.04)" strokeWidth="2"/>
            <circle cx="60" cy="60" r="50" fill="none" stroke={`url(#conn-${theme.id})`} strokeWidth="2.5"
                    strokeLinecap="round" strokeDasharray="150 160"
                    style={{ filter: `drop-shadow(0 0 6px ${theme.grad.to}aa)` }}/>
          </svg>
          <div style={{
            position: 'absolute', inset: 0, display: 'flex', alignItems: 'center', justifyContent: 'center',
            fontFamily: theme.mono, fontSize: 11, letterSpacing: 1.5, color: theme.fgDim,
          }}>
            <span style={{ animation: 'mc-blink 1s infinite' }}>●</span>
          </div>
        </div>

        <div style={{
          textAlign: 'center', fontSize: 18, fontWeight: 600,
          letterSpacing: -0.3, color: theme.fg, marginBottom: 6,
        }}>
          正在协商 RTSP 会话…
        </div>
        <div style={{
          textAlign: 'center', fontFamily: theme.mono, fontSize: 11,
          color: theme.mute, marginBottom: 28, letterSpacing: 0.3,
          whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis',
        }}>{url}</div>

        {/* Step ladder */}
        <div style={{
          background: 'rgba(0,0,0,0.3)',
          border: `1px solid ${theme.chrome.border}`, borderRadius: 10, padding: 14,
          display: 'flex', flexDirection: 'column', gap: 2,
        }}>
          {steps.map((s, i) => (
            <div key={s.id} style={{
              display: 'flex', alignItems: 'center', gap: 12,
              padding: '7px 10px', borderRadius: 6,
              background: s.active ? 'rgba(255,255,255,0.03)' : 'transparent',
            }}>
              <div style={{
                width: 18, height: 18, borderRadius: '50%',
                display: 'flex', alignItems: 'center', justifyContent: 'center',
                background: s.done
                  ? `linear-gradient(135deg, ${theme.grad.from}, ${theme.grad.to})`
                  : s.active ? 'rgba(255,255,255,0.05)' : 'transparent',
                border: s.active ? `1px solid ${theme.grad.to}` : `1px solid ${theme.chrome.border}`,
                flexShrink: 0,
              }}>
                {s.done && <svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke={theme.grad.onGrad} strokeWidth="3" strokeLinecap="round" strokeLinejoin="round"><path d="M5 13l4 4L19 7"/></svg>}
                {s.active && <div style={{
                  width: 5, height: 5, borderRadius: '50%', background: theme.grad.to,
                  boxShadow: `0 0 6px ${theme.grad.to}`, animation: 'mc-blink 0.8s infinite',
                }}/>}
              </div>
              <div style={{
                fontFamily: theme.mono, fontSize: 11.5, letterSpacing: 0.6,
                color: s.done ? theme.fgDim : s.active ? theme.fg : theme.mute,
                fontWeight: s.active ? 600 : 400, flex: 1,
              }}>{s.label}</div>
              <div style={{
                fontFamily: theme.mono, fontSize: 10, letterSpacing: 0.3,
                color: s.done ? theme.ok : s.active ? theme.fgDim : theme.mute,
              }}>
                {s.done ? `${12 + i * 7}ms` : s.active ? '...' : '—'}
              </div>
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}

// ─── Error state — diagnostic card ─────────────────────────────────────
function ErrorStage({ theme, url }) {
  return (
    <div style={{
      flex: 1, position: 'relative', display: 'flex',
      alignItems: 'center', justifyContent: 'center', overflow: 'hidden',
    }}>
      <Atmosphere theme={theme} variant="error"/>
      <div style={{ position: 'relative', zIndex: 1, width: 560, maxWidth: '90%' }}>
        {/* Icon */}
        <div style={{
          width: 64, height: 64, margin: '0 auto 24px',
          borderRadius: 16, display: 'flex', alignItems: 'center', justifyContent: 'center',
          background: `linear-gradient(135deg, ${theme.err}, ${theme.err}88)`,
          boxShadow: `0 0 0 1px ${theme.err}44, 0 0 40px -6px ${theme.err}88`,
        }}>
          <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="#fff" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
            {I.warn}
          </svg>
        </div>

        <div style={{
          textAlign: 'center', fontSize: 22, fontWeight: 700,
          letterSpacing: -0.4, color: theme.fg, marginBottom: 8,
        }}>连接超时</div>
        <div style={{
          textAlign: 'center', fontFamily: theme.mono, fontSize: 12,
          color: theme.fgDim, marginBottom: 24,
        }}>
          <span style={{ color: theme.err }}>avformat_open_input</span>
          <span style={{ color: theme.mute }}> → </span>
          <span>ETIMEDOUT (-110)</span>
        </div>

        {/* URL echo */}
        <div style={{
          padding: '10px 14px', marginBottom: 16,
          background: 'rgba(0,0,0,0.35)', borderRadius: 8,
          border: `1px solid ${theme.chrome.border}`,
          fontFamily: theme.mono, fontSize: 11.5, color: theme.fgDim,
          whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis',
        }}>{url}</div>

        {/* Checklist */}
        <div style={{
          padding: 14, borderRadius: 8,
          background: 'rgba(0,0,0,0.2)',
          border: `1px solid ${theme.chrome.border}`,
          marginBottom: 18,
        }}>
          <div style={{
            fontSize: 10, letterSpacing: 1.5, color: theme.mute,
            fontFamily: theme.mono, textTransform: 'uppercase',
            fontWeight: 600, marginBottom: 10,
          }}>诊断建议</div>
          {[
            ['RTSP 端口 554 未开放或被防火墙拦截', true],
            ['凭据 (username:password) 不正确', false],
            ['传输协议尝试 UDP (可在设置中切换)', false],
            ['设备在线 — 检查 IP 与网络可达性', true],
          ].map(([t, hot], i) => (
            <div key={i} style={{
              display: 'flex', alignItems: 'center', gap: 10,
              padding: '6px 0', fontSize: 12, color: theme.fgDim, lineHeight: 1.4,
            }}>
              <div style={{
                width: 5, height: 5, borderRadius: '50%', flexShrink: 0,
                background: hot ? theme.warn : theme.mute,
                boxShadow: hot ? `0 0 6px ${theme.warn}66` : 'none',
              }}/>
              <span>{t}</span>
            </div>
          ))}
        </div>

        {/* Actions */}
        <div style={{ display: 'flex', gap: 10, justifyContent: 'center' }}>
          <button style={{
            height: 36, padding: '0 20px', borderRadius: 8, border: 'none',
            background: `linear-gradient(135deg, ${theme.grad.from}, ${theme.grad.to})`,
            color: theme.grad.onGrad, fontFamily: theme.ui, fontSize: 12.5,
            fontWeight: 600, letterSpacing: 0.4, cursor: 'pointer',
            display: 'flex', alignItems: 'center', gap: 7,
            boxShadow: `0 6px 20px -4px ${theme.grad.to}55`,
          }}>
            <Icon d={I.retry} size={13}/> 重试
          </button>
          <button style={{
            height: 36, padding: '0 20px', borderRadius: 8,
            background: 'rgba(255,255,255,0.03)', color: theme.fg,
            border: `1px solid ${theme.chrome.border}`,
            fontFamily: theme.ui, fontSize: 12.5, fontWeight: 500,
            cursor: 'pointer', display: 'flex', alignItems: 'center', gap: 7,
          }}>
            <Icon d={I.gear} size={13}/> 传输设置
          </button>
        </div>
      </div>
    </div>
  );
}

// ─── Playing state — live video + HUD ──────────────────────────────────
function PlayingStage({ theme, stats, hudOpen, setHudOpen }) {
  return (
    <div style={{
      flex: 1, position: 'relative',
      display: 'flex', flexDirection: 'column',
      overflow: 'hidden', background: '#000',
    }}>
      {/* Fake video content: ambient wash to suggest live feed */}
      <FakeVideo theme={theme}/>

      {/* Top-left: live pill */}
      <div style={{
        position: 'absolute', top: 14, left: 14, zIndex: 3,
        display: 'flex', alignItems: 'center', gap: 8,
        padding: '6px 10px', borderRadius: 6,
        background: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(8px)',
        border: `1px solid rgba(255,255,255,0.08)`,
        fontFamily: theme.mono, fontSize: 10.5, letterSpacing: 1, color: theme.fg,
      }}>
        <span style={{
          width: 6, height: 6, borderRadius: '50%',
          background: theme.err, boxShadow: `0 0 8px ${theme.err}`,
          animation: 'mc-blink 1.4s infinite',
        }}/>
        LIVE
        <span style={{ color: theme.mute, marginLeft: 2 }}>· {fmtTime(stats.uptimeMs)}</span>
      </div>

      {/* Top-right: minimal latency badge (always visible) */}
      <div style={{
        position: 'absolute', top: 14, right: 14, zIndex: 3,
        display: 'flex', alignItems: 'center', gap: 10,
        padding: '8px 14px', borderRadius: 8,
        background: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(10px)',
        border: `1px solid rgba(255,255,255,0.08)`,
      }}>
        <div style={{
          fontFamily: theme.mono, fontSize: 9, letterSpacing: 1.5,
          color: theme.mute, textTransform: 'uppercase', fontWeight: 600,
        }}>latency</div>
        <div style={{
          fontFamily: theme.mono, fontSize: 20, fontWeight: 700,
          letterSpacing: -0.5, lineHeight: 1,
          fontVariantNumeric: 'tabular-nums',
        }}>
          <GradText theme={theme}>{stats.latencyMs}</GradText>
          <span style={{ color: theme.mute, fontSize: 11, fontWeight: 500, marginLeft: 2 }}>ms</span>
        </div>
      </div>

      {/* Center-bottom controls */}
      <div style={{
        position: 'absolute', bottom: 14, left: 14, right: 14, zIndex: 3,
        display: 'flex', alignItems: 'center', gap: 10,
      }}>
        <div style={{
          display: 'flex', alignItems: 'center', gap: 4,
          padding: 4, borderRadius: 8,
          background: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(10px)',
          border: `1px solid rgba(255,255,255,0.08)`,
        }}>
          {[I.snap, I.vol, I.exp].map((d, i) => (
            <button key={i} style={{
              width: 30, height: 30, borderRadius: 6,
              background: 'transparent', border: 'none', color: theme.fgDim,
              display: 'flex', alignItems: 'center', justifyContent: 'center',
              cursor: 'pointer',
            }}><Icon d={d} size={14}/></button>
          ))}
        </div>

        <div style={{ flex: 1 }}/>

        {/* HUD toggle */}
        <button onClick={() => setHudOpen && setHudOpen(!hudOpen)} style={{
          height: 34, padding: '0 14px', borderRadius: 8,
          background: hudOpen
            ? `linear-gradient(135deg, ${theme.grad.from}, ${theme.grad.to})`
            : 'rgba(0,0,0,0.55)',
          color: hudOpen ? theme.grad.onGrad : theme.fgDim,
          border: hudOpen ? 'none' : `1px solid rgba(255,255,255,0.08)`,
          backdropFilter: 'blur(10px)',
          fontFamily: theme.mono, fontSize: 10.5, letterSpacing: 1, fontWeight: 600,
          display: 'flex', alignItems: 'center', gap: 8, cursor: 'pointer',
          textTransform: 'uppercase',
        }}>
          <Icon d={I.info} size={13}/>
          HUD
        </button>
      </div>

      {/* HUD side panel */}
      {hudOpen && <HudPanel theme={theme} stats={stats}/>}
    </div>
  );
}

// ─── Fake video — subtle moving gradient + horizontal scan, pretends to be a feed
function FakeVideo({ theme }) {
  return (
    <div style={{ position: 'absolute', inset: 0, overflow: 'hidden' }}>
      {/* Base scene gradient */}
      <div style={{
        position: 'absolute', inset: 0,
        background: `linear-gradient(180deg, #0a0d12 0%, #141922 60%, #08090c 100%)`,
      }}/>
      {/* Simulated scene elements — abstract city-lights / sky */}
      <div style={{
        position: 'absolute', left: '10%', top: '20%', width: '80%', height: '45%',
        background: `radial-gradient(ellipse at 50% 100%, ${theme.grad.from}22, transparent 70%)`,
      }}/>
      <div style={{
        position: 'absolute', left: 0, right: 0, bottom: 0, height: '40%',
        background: `linear-gradient(0deg, ${theme.grad.to}18, transparent)`,
      }}/>
      {/* Faux scanlines */}
      <div style={{
        position: 'absolute', inset: 0, opacity: 0.12,
        backgroundImage: 'repeating-linear-gradient(0deg, rgba(255,255,255,0.04) 0px, rgba(255,255,255,0.04) 1px, transparent 1px, transparent 3px)',
      }}/>
      {/* Corner vignette */}
      <div style={{
        position: 'absolute', inset: 0,
        background: 'radial-gradient(ellipse at center, transparent 60%, rgba(0,0,0,0.5) 100%)',
      }}/>
    </div>
  );
}

// ─── HUD floating panel ────────────────────────────────────────────────
function HudPanel({ theme, stats }) {
  return (
    <div style={{
      position: 'absolute', top: 60, right: 14, zIndex: 4,
      width: 280, padding: 16, borderRadius: 12,
      background: 'rgba(10,12,16,0.82)', backdropFilter: 'blur(16px)',
      border: `1px solid rgba(255,255,255,0.08)`,
      boxShadow: '0 20px 60px rgba(0,0,0,0.5)',
      display: 'flex', flexDirection: 'column', gap: 12,
    }}>
      <div style={{
        display: 'flex', alignItems: 'center', justifyContent: 'space-between',
        paddingBottom: 4,
      }}>
        <div style={{
          fontSize: 10, letterSpacing: 1.8, color: theme.mute,
          fontFamily: theme.mono, textTransform: 'uppercase', fontWeight: 700,
        }}>Telemetry</div>
        <div style={{
          fontSize: 9, letterSpacing: 1, color: theme.ok,
          fontFamily: theme.mono, fontWeight: 600,
          display: 'flex', alignItems: 'center', gap: 4,
        }}>
          <span style={{
            width: 4, height: 4, borderRadius: '50%',
            background: theme.ok, boxShadow: `0 0 4px ${theme.ok}`,
          }}/>
          LIVE
        </div>
      </div>

      {/* Hero latency (smaller than empty-stage) */}
      <div style={{ display: 'flex', justifyContent: 'center', margin: '-4px 0' }}>
        <LatencyHero theme={theme} ms={stats.latencyMs}/>
      </div>

      {/* Grid of stats */}
      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 8 }}>
        <StatBlock theme={theme} label="FPS" value={stats.fps} unit="hz" sparkline={<Sparkline theme={theme} points={stats.fpsSeries}/>}/>
        <StatBlock theme={theme} label="Bitrate" value={stats.bitrate} unit="Mbps" sparkline={<Sparkline theme={theme} points={stats.brSeries}/>}/>
      </div>

      {/* Key-value list */}
      <div style={{
        display: 'flex', flexDirection: 'column', gap: 2,
        padding: 12, borderRadius: 8,
        background: 'rgba(0,0,0,0.35)',
        border: `1px solid ${theme.chrome.border}`,
        fontFamily: theme.mono, fontSize: 11,
      }}>
        {[
          ['codec',        `H.264 · ${stats.profile}`],
          ['resolution',   `${stats.w}×${stats.h}`],
          ['transport',    'TCP'],
          ['hwaccel',      'D3D11VA'],
          ['drop',         `${stats.dropped} frames`],
          ['jitter',       `${stats.jitter} ms`],
        ].map(([k, v]) => (
          <div key={k} style={{
            display: 'flex', justifyContent: 'space-between',
            padding: '3px 0',
          }}>
            <span style={{ color: theme.mute, letterSpacing: 0.3 }}>{k}</span>
            <span style={{ color: theme.fg, fontVariantNumeric: 'tabular-nums' }}>{v}</span>
          </div>
        ))}
      </div>
    </div>
  );
}

Object.assign(window, { EmptyStage, ConnectingStage, ErrorStage, PlayingStage });
