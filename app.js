/* ==========================================================================
   VIVERO MONITOR - LÓGICA DE LA APLICACIÓN (JAVASCRIPT)
   ========================================================================== */

/* ==========================================================================
   1. CATÁLOGO DE ESPECIES Y CONFIGURACIÓN AGRONÓMICA
   ========================================================================== */
const P = [
  { id: 'lechuga', n: 'Lechuga', t: [15, 22], h: [60, 80], luz: 'media-alta', riego: 'frecuente', nota: 'Sobre 25 °C tiende a espigarse.' },
  { id: 'tomate', n: 'Tomate', t: [20, 27], h: [60, 75], luz: 'alta', riego: 'regular', nota: 'Humedad muy alta favorece hongos.' },
  { id: 'albahaca', n: 'Albahaca', t: [20, 28], h: [40, 60], luz: 'alta', riego: 'moderado', nota: 'Muy sensible al frío.' },
  { id: 'cilantro', n: 'Cilantro', t: [15, 22], h: [50, 70], luz: 'media', riego: 'regular', nota: 'Con calor sube a flor rápido.' },
  { id: 'fresa', n: 'Fresa', t: [15, 25], h: [60, 75], luz: 'alta', riego: 'constante', nota: 'Cuidar la humedad sobre el fruto.' },
  { id: 'pimenton', n: 'Pimentón', t: [21, 27], h: [60, 70], luz: 'alta', riego: 'regular', nota: 'Frena su crecimiento bajo 15 °C.' },
  { id: 'espinaca', n: 'Espinaca', t: [10, 20], h: [50, 70], luz: 'media', riego: 'frecuente', nota: 'Prefiere clima fresco.' },
  { id: 'orquidea', n: 'Orquídea', t: [18, 28], h: [50, 70], luz: 'indirecta', riego: 'espaciado', nota: 'Necesita buena ventilación.' },
  { id: 'helecho', n: 'Helecho', t: [16, 24], h: [70, 90], luz: 'baja-media', riego: 'constante', nota: 'No tolera el ambiente seco.' },
  { id: 'suculenta', n: 'Suculentas', t: [18, 30], h: [30, 50], luz: 'alta', riego: 'muy espaciado', nota: 'El exceso de humedad las pudre.' },
  { id: 'romero', n: 'Romero', t: [15, 25], h: [40, 60], luz: 'alta', riego: 'poco', nota: 'No tolera el encharcamiento.' },
  { id: 'menta', n: 'Menta', t: [15, 25], h: [60, 80], luz: 'media', riego: 'frecuente', nota: 'Mejor en maceta aparte.' }
];

// Márgenes de tolerancia entre advertencia y condición crítica
const CT = 5;   // Margen de temperatura (°C)
const CH = 10;  // Margen de humedad (%)

/* ==========================================================================
   2. CAPA DE SERVICIO: FIREBASE (con modo demo si no hay configuración)
   ========================================================================== */
const REAL = FBCFG.apiKey !== 'TU_API_KEY';
let fb = null;       // Módulos de Firebase ya cargados
let subOk = false;   // Evita suscribirse dos veces a la base de datos

async function iniciarFB() {
  if (fb) return;
  const base = 'https://www.gstatic.com/firebasejs/10.12.2/';
  const m = await Promise.all([
    import(base + 'firebase-app.js'),
    import(base + 'firebase-auth.js'),
    import(base + 'firebase-database.js')
  ]);
  const app = m[0].initializeApp(FBCFG);
  fb = { au: m[1], d: m[2], auth: m[1].getAuth(app), db: m[2].getDatabase(app) };
}

const API = {
  login: async (email, pass) => {
    if (REAL) {
      await iniciarFB();
      const cred = await fb.au.signInWithEmailAndPassword(fb.auth, email, pass);
      return { email: cred.user.email };
    }
    if (!email.includes('@') || pass.length < 4) {
      throw new Error('Revisa tu correo y contraseña (mínimo 4 caracteres).');
    }
    return { email };
  }
};

// "TEMPERATURA ALTA" -> "Temperatura alta"
const nom = n => (n ? n[0] + n.slice(1).toLowerCase() : '');

/** Escucha en tiempo real lo que publica la ESP32 y la configuración compartida */
function suscribir() {
  if (subOk || !REAL) return;
  subOk = true;
  const d = fb.d;
  const R = ruta => d.ref(fb.db, ruta);

  d.onValue(R('lecturas/actual'), snap => {
    const v = snap.val();
    if (!v) return;
    S.live = v;
    if (v.t != null) { S.T = v.t; S.H = v.h; }
    upd();
  });

  d.onValue(d.query(R('historial'), d.limitToLast(90)), snap => {
    S.hist = Object.values(snap.val() || {}).filter(x => x.t != null).map(x => [x.t, x.h]);
    upd();
  });

  d.onValue(d.query(R('alarmas'), d.limitToLast(20)), snap => {
    S.log = Object.values(snap.val() || {}).reverse().map(x => ({
      t: new Date(x.ts).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }),
      s: x.sev,
      m: nom(x.alarma) + (x.sev ? '' : ' (normalizado)')
    }));
    upd();
  });

  d.onValue(R('config/plantas'), snap => {
    S.sel = snap.val() || [];
    if (S.user) draw();
  });
}

/** Guarda las plantas elegidas y los umbrales que descargará la ESP32 */
function publicarConfig() {
  if (!REAL || !fb) return;
  const d = fb.d;
  d.set(d.ref(fb.db, 'config/plantas'), S.sel);
  const u = S.sel.length ? umbrales() : null;
  if (u) d.set(d.ref(fb.db, 'config/umbrales'), u);
}

/** Estado que muestra el panel: el de la ESP32 en modo real, o el simulado */
function evalPanel() {
  const e = evaluar(S.T, S.H);
  if (!REAL) return e;
  if (!S.live) return { ...e, off: true };
  const sev = S.live.sev || 0;
  return {
    s: sev,
    m: sev ? nom(S.live.alarma) : e.m,
    off: Date.now() - S.live.ts > 45000   // sin datos hace más de 45 s
  };
}

/* ==========================================================================
   3. ESTADO GLOBAL Y PERSISTENCIA (LOCALSTORAGE)
   ========================================================================== */
let S = {
  view: 'panel',  // Vista activa: 'panel' | 'plantas' | 'recs'
  sel: [],        // IDs de las plantas seleccionadas
  T: 24,          // Temperatura actual simulada (°C)
  H: 60,          // Humedad actual simulada (%)
  tT: 24,         // Temperatura objetivo (slider)
  tH: 60,         // Humedad objetivo (slider)
  hist: [],       // Historial de lecturas recientes [T, H]
  log: [],        // Registro cronológico de alarmas
  sev: 0,         // Severidad: 0 (normal), 1 (advertencia), 2 (crítica)
  user: null,     // Datos del usuario logueado
  first: true,    // Bandera para animación de arcos
  live: null      // Última lectura publicada por la ESP32 (Firebase)
};

// Restaurar sesión y selección previa desde LocalStorage
try {
  const saved = JSON.parse(localStorage.getItem('vm') || '{}');
  S.sel = saved.sel || [];
  S.user = REAL ? null : (saved.user || null);
} catch (e) { }

const save = () => {
  try {
    localStorage.setItem('vm', JSON.stringify({ sel: S.sel, user: S.user }));
  } catch (e) { }
};

// Funciones utilitarias
const $ = selector => document.querySelector(selector);
const f1 = num => num.toFixed(1);

/* ==========================================================================
   4. LÓGICA DE NEGOCIO AGRONÓMICA
   ========================================================================== */

/**
 * Calcula la intersección de rangos óptimos de clima según las plantas elegidas
 */
function zona() {
  const L = S.sel.map(id => P.find(p => p.id === id)).filter(Boolean);
  if (!L.length) {
    return { def: true, t: [12, 30], h: [50, 85], ok: true, L };
  }
  const t = [Math.max(...L.map(p => p.t[0])), Math.min(...L.map(p => p.t[1]))];
  const h = [Math.max(...L.map(p => p.h[0])), Math.min(...L.map(p => p.h[1]))];
  return { t, h, ok: t[0] <= t[1] && h[0] <= h[1], L };
}

/**
 * Detecta incompatibilidades climáticas entre pares de plantas seleccionadas
 */
function conflictos(L) {
  const c = [];
  for (let i = 0; i < L.length; i++) {
    for (let j = i + 1; j < L.length; j++) {
      const a = L[i], b = L[j];
      if (Math.max(a.t[0], b.t[0]) > Math.min(a.t[1], b.t[1])) {
        c.push(`${a.n} y ${b.n} no comparten rango de temperatura.`);
      }
      if (Math.max(a.h[0], b.h[0]) > Math.min(a.h[1], b.h[1])) {
        c.push(`${a.n} y ${b.n} no comparten rango de humedad.`);
      }
    }
  }
  return c;
}

/**
 * Evalúa la condición actual vs los límites recomendados
 */
function evaluar(T, H) {
  const z = zona();
  if (!z.ok) return { s: 0, m: 'Sin rango común' };

  const dT = T < z.t[0] ? z.t[0] - T : T > z.t[1] ? T - z.t[1] : 0;
  const dH = H < z.h[0] ? z.h[0] - H : H > z.h[1] ? H - z.h[1] : 0;
  const m = [];

  if (dT) m.push(T < z.t[0] ? 'Temperatura baja' : 'Temperatura alta');
  if (dH) m.push(H < z.h[0] ? 'Humedad baja' : 'Humedad alta');

  const s = (dT > CT || dH > CH) ? 2 : (dT || dH) ? 1 : 0;
  const mensaje = m.join(' y ') || (z.def ? 'Clima estable' : 'Condiciones ideales');
  return { s, m: mensaje };
}

/**
 * Umbrales calculados para enviar al microcontrolador (ESP32)
 */
function umbrales() {
  const z = zona();
  if (!z.ok) return null;
  return {
    t_baja_adv: z.t[0],
    t_baja_crit: z.t[0] - CT,
    t_alta_adv: z.t[1],
    t_alta_crit: z.t[1] + CT,
    h_baja_adv: z.h[0],
    h_baja_crit: z.h[0] - CH,
    h_alta_adv: z.h[1],
    h_alta_crit: z.h[1] + CH
  };
}

/**
 * Promedio de lecturas climáticas recientes
 */
function avg() {
  const a = S.hist.slice(-30);
  return a.length
    ? [a.reduce((acc, x) => acc + x[0], 0) / a.length, a.reduce((acc, x) => acc + x[1], 0) / a.length]
    : [S.T, S.H];
}

/**
 * Alterna la selección de una planta en la lista
 */
function toggle(id) {
  const idx = S.sel.indexOf(id);
  if (idx < 0) {
    S.sel.push(id);
  } else {
    S.sel.splice(idx, 1);
  }
  save();
  publicarConfig();
  S.sev = -1;
  draw();
}

/* ==========================================================================
   5. SIMULADOR DE LECTURAS DE SENSORES
   ========================================================================== */
function tick() {
  // Inercia hacia el target de los sliders + ruido estocástico
  S.T += (S.tT - S.T) * 0.15 + (Math.random() - 0.5) * 0.3;
  S.H += (S.tH - S.H) * 0.15 + (Math.random() - 0.5) * 0.8;

  S.hist.push([S.T, S.H]);
  if (S.hist.length > 90) S.hist.shift();

  const e = evaluar(S.T, S.H);
  if (e.s !== S.sev) {
    const hora = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
    S.log.unshift({ t: hora, s: e.s, m: e.s ? e.m : 'Normalizado' });
    S.log = S.log.slice(0, 20);
    S.sev = e.s;
  }

  if (S.user) upd();
}

/* ==========================================================================
   6. RENDERIZADO VISUAL: TACÓMETROS, BARRAS Y GRÁFICOS SVG
   ========================================================================== */
const pol = (angle, radius = 78) => [
  100 + radius * Math.cos(angle * Math.PI / 180),
  100 + radius * Math.sin(angle * Math.PI / 180)
];

const arc = (start, end) => {
  const p = pol(start);
  const q = pol(end);
  return `M${p[0]} ${p[1]} A78 78 0 ${end - start > 180 ? 1 : 0} 1 ${q[0]} ${q[1]}`;
};

function ring(value, low, high, min, max, unit, severity) {
  const mapAngle = x => 135 + Math.max(0, Math.min(1, (x - min) / (max - min))) * 270;
  const marker = pol(mapAngle(value));
  const col = ['fern', 'amb', 'cor'][severity];

  return `
    <svg viewBox="0 0 200 200" class="ring" role="img" aria-label="${f1(value)} ${unit}, rango ideal ${low} a ${high}">
      <path d="${arc(135, 405)}" fill="none" stroke="rgba(255,255,255,.16)" stroke-width="12" stroke-linecap="round"/>
      <path class="${S.first ? 'dr' : ''}" pathLength="1" d="${arc(mapAngle(low), mapAngle(high))}" fill="none" stroke="url(#fg)" stroke-width="12" stroke-linecap="round"/>
      <circle cx="${marker[0]}" cy="${marker[1]}" r="10" fill="var(--${col})" stroke="#fff" stroke-width="3"/>
      <text x="100" y="108" text-anchor="middle" class="num">${value.toFixed(unit === '°C' ? 1 : 0)}</text>
      <text x="100" y="132" text-anchor="middle" class="unit">${unit}</text>
    </svg>
  `;
}

const mini = (low, high, min, max, cls) => `
  <div class="mb ${cls || ''}">
    <i style="left:${(low - min) / (max - min) * 100}%; width:${(high - low) / (max - min) * 100}%"></i>
  </div>
`;

/* ==========================================================================
   7. ILUSTRACIONES PROCEDURALES DE PLANTAS (SVG DINÁMICO)
   ========================================================================== */
const POTS = {
  terra: ['#c9703f', '#a85a30'],
  negro: ['#2a2a30', '#15151a'],
  piedra: ['#cdbfa6', '#a79983'],
  blanco: ['#eef0ea', '#c9ccc4'],
  madera: ['#9a6a44', '#7a4f30']
};

// Parámetros de generación botánica: [hojas, largo, ancho, apertura, tono_hsl, tipo_maceta]
const VIS = {
  lechuga: [10, 38, 14, 75, 100, 'terra'],
  tomate: [12, 34, 10, 62, 118, 'madera'],
  albahaca: [14, 30, 10, 60, 112, 'terra'],
  cilantro: [11, 40, 7, 55, 128, 'blanco'],
  fresa: [9, 38, 13, 68, 105, 'piedra'],
  pimenton: [6, 52, 17, 48, 122, 'terra'],
  espinaca: [8, 46, 12, 56, 110, 'negro'],
  orquidea: [4, 58, 19, 44, 140, 'blanco'],
  helecho: [11, 62, 6, 78, 135, 'terra'],
  suculenta: [9, 50, 9, 40, 150, 'negro'],
  romero: [13, 46, 4, 52, 138, 'piedra'],
  menta: [16, 32, 11, 66, 108, 'terra']
};

function planta(id) {
  const [n, len, wid, sp, hu, pot] = VIS[id];
  const pc = POTS[pot];
  let hojasSvg = '';

  for (let i = 0; i < n; i++) {
    const a = n > 1 ? -sp + (2 * sp * i) / (n - 1) : 0;
    const l = len * (0.78 + 0.22 * Math.abs(Math.sin(i * 2.3)));
    const w = wid * (0.85 + 0.15 * Math.cos(i * 1.7));
    const c = `hsl(${hu + (i % 3) * 6}, 50%, ${30 + (i % 3) * 8}%)`;

    hojasSvg += `
      <g transform="translate(60 146) rotate(${a.toFixed(1)})">
        <path d="M0 0 C${-w} ${-l * 0.3} ${-w * 0.7} ${-l * 0.8} 0 ${-l} C${w * 0.7} ${-l * 0.8} ${w} ${-l * 0.3} 0 0Z" fill="${c}"/>
        <path d="M0 -4 V${-l * 0.85}" stroke="rgba(255,255,255,.28)" stroke-width="1"/>
      </g>
    `;
  }

  return `
    <svg viewBox="0 0 120 200" aria-hidden="true">
      <ellipse cx="60" cy="191" rx="50" ry="7" fill="rgba(0,0,0,.28)"/>
      <rect x="12" y="180" width="96" height="9" rx="4.5" fill="#3d7a52"/>
      <rect x="12" y="180" width="96" height="3" rx="1.5" fill="rgba(255,255,255,.25)"/>
      ${hojasSvg}
      <path d="M40 146 H80 L74 180 H46 Z" fill="${pc[0]}"/>
      <path d="M50 150 L48 177" stroke="rgba(255,255,255,.2)" stroke-width="3" stroke-linecap="round"/>
      <rect x="36" y="142" width="48" height="9" rx="3" fill="${pc[1]}"/>
    </svg>
  `;
}

/* ==========================================================================
   8. VISTAS DE LA APLICACIÓN
   ========================================================================== */

/**
 * Vista 1: Panel Principal de Monitoreo
 */
function panel() {
  return `
    <div id="hd"></div>
    <div class="rings">
      <div class="glass card" id="rt"></div>
      <div class="glass card" id="rh"></div>
    </div>
    <div class="stack">
      <div class="glass card">
        <div class="head">
          <h3>Tendencia</h3>
          <span class="mu">últimos minutos</span>
        </div>
        <div id="sp"></div>
      </div>
      <div class="glass card">
        <h3 style="margin-bottom: 6px;">Alarmas</h3>
        <div id="lg" class="log"></div>
      </div>
      <details class="glass card ${REAL ? 'hide' : ''}">
        <summary>Simular condiciones</summary>
        <label>Temperatura: <b>${S.tT} °C</b></label>
        <input type="range" min="0" max="45" value="${S.tT}" oninput="S.tT = +this.value; this.previousElementSibling.lastChild.textContent = this.value + ' °C'">
        <label>Humedad: <b>${S.tH} %</b></label>
        <input type="range" min="10" max="100" value="${S.tH}" oninput="S.tH = +this.value; this.previousElementSibling.lastChild.textContent = this.value + ' %'">
      </details>
    </div>
  `;
}

/**
 * Actualiza el contenido reactivo del Panel en tiempo real
 */
function upd() {
  if (S.view !== 'panel' || !$('#hd')) return;

  const z = zona();
  const zr = z.ok ? z : { t: [12, 30], h: [50, 85] };
  const e = evalPanel();
  const labels = ['Normal', 'Advertencia', 'Crítica'];

  // Cabecera de estado
  $('#hd').innerHTML = `
    <h1 class="hero">${e.m}</h1>
    <p class="mu" style="margin: 10px 0 12px;">
      ${z.def ? 'Aún no elegiste plantas. Hazlo en Mis plantas para personalizar las alertas.' : 'Para ' + z.L.map(p => p.n).join(', ') + '.'}
    </p>
    <span class="pill s${e.s}">
      <span class="dot"></span>${labels[e.s]}
    </span>
    ${e.off ? '<div class="warn" style="margin-top: 14px;">Sin datos recientes del sensor. Revisa que la ESP32 esté encendida y con Wi-Fi.</div>' : ''}
  `;

  // Tacómetros
  $('#rt').innerHTML = ring(S.T, zr.t[0], zr.t[1], 0, 45, '°C', e.s) + `<p class="mu">Temperatura<br>ideal ${zr.t[0]} a ${zr.t[1]} °C</p>`;
  $('#rh').innerHTML = ring(S.H, zr.h[0], zr.h[1], 10, 100, '%', e.s) + `<p class="mu">Humedad<br>ideal ${zr.h[0]} a ${zr.h[1]} %</p>`;

  // Gráfica de Sparkline / Tendencia
  const mapX = i => (i / 89) * 300;
  const mapY = (k, min, max) => i => 100 - ((S.hist[i][k] - min) / (max - min)) * 96 - 2;
  const points = (k, min, max) => S.hist.map((_, i) => `${mapX(i).toFixed(1)},${mapY(k, min, max)(i).toFixed(1)}`).join(' ');

  $('#sp').innerHTML = S.hist.length < 2 ? '<p class="mu">Recopilando datos…</p>' : `
    <svg viewBox="0 0 300 100" width="100%" height="120" preserveAspectRatio="none">
      <path d="M0,100 L${points(0, 0, 45)} L${mapX(S.hist.length - 1)},100Z" fill="url(#ar)"/>
      <polyline fill="none" stroke="#a6d96b" stroke-width="2" vector-effect="non-scaling-stroke" points="${points(0, 0, 45)}"/>
      <polyline fill="none" stroke="#7fd0ee" stroke-width="2" vector-effect="non-scaling-stroke" points="${points(1, 10, 100)}"/>
    </svg>
    <p class="mu" style="margin-top: 8px;">
      <span style="color: var(--fern);">●</span> Temperatura &nbsp; 
      <span style="color: var(--aqua);">●</span> Humedad
    </p>
  `;

  // Log de alarmas
  $('#lg').innerHTML = S.log.length
    ? S.log.map(x => `
        <div>
          <span class="dot s${x.s}"></span>
          <span>${x.m}</span>
          <span>${x.t}</span>
        </div>
      `).join('')
    : '<p class="mu">Sin alarmas por ahora.</p>';

  S.first = false;
}

/**
 * Vista 2: Mis Plantas y Selección de Catálogo
 */
function plantas() {
  const z = zona();
  const cf = conflictos(z.L);
  const u = umbrales();

  return `
    <div class="glass card" style="margin-bottom: 14px;">
      <h3>Zona calculada</h3>
      ${z.def
      ? '<p class="mu" style="margin-top: 6px;">Elige las plantas del vivero. Calculamos el clima donde todas están bien.</p>'
      : z.ok
        ? `
            <div class="mu" style="margin-top: 12px;">Temperatura común: <b style="color:var(--tx)">${z.t[0]} a ${z.t[1]} °C</b></div>
            ${mini(z.t[0], z.t[1], 0, 45)}
            <div class="mu">Humedad común: <b style="color:var(--tx)">${z.h[0]} a ${z.h[1]} %</b></div>
            ${mini(z.h[0], z.h[1], 10, 100, 'h')}
          `
        : '<p style="margin-top: 6px;">No hay un rango común entre estas plantas.</p>'
    }
      ${cf.map(c => `<div class="warn">${c} Considera separarlas en zonas distintas.</div>`).join('')}
      ${u ? `<details style="margin-top: 12px;"><summary class="mu">Umbrales que recibiría la ESP32</summary><pre>${JSON.stringify(u, null, 2)}</pre></details>` : ''}
    </div>

    <div class="pg">
      ${P.map(p => {
      const isSelected = S.sel.includes(p.id);
      return `
          <div class="glass pc ${isSelected ? 'sel' : ''}" 
               role="button" 
               tabindex="0" 
               aria-label="Ver detalles de ${p.n}" 
               onclick="abrir('${p.id}')" 
               onkeydown="if(event.key === 'Enter') abrir('${p.id}')">
            ${isSelected ? '<span class="badge">✓</span>' : ''}
            ${planta(p.id)}
            <div class="ft">
              <div>
                <h3>${p.n}</h3>
                <div class="mu" style="font-size: .78rem;">${p.t[0]} a ${p.t[1]} °C</div>
              </div>
              <button class="tg" 
                      aria-pressed="${isSelected}" 
                      aria-label="${isSelected ? 'Quitar' : 'Agregar'} ${p.n}" 
                      onclick="event.stopPropagation(); toggle('${p.id}')">
                ${isSelected ? '✓' : '+'}
              </button>
            </div>
          </div>
        `;
    }).join('')}
    </div>
  `;
}

/**
 * Vista 3: Recomendaciones según clima actual
 */
function recs() {
  const [T, H] = avg();
  const ranking = P.map(p => {
    const dT = T < p.t[0] ? p.t[0] - T : T > p.t[1] ? T - p.t[1] : 0;
    const dH = H < p.h[0] ? p.h[0] - H : H > p.h[1] ? H - p.h[1] : 0;
    return {
      p,
      dT,
      dH,
      match: Math.max(0, Math.round(100 - dT * 9 - dH * 2.5))
    };
  }).sort((a, b) => b.match - a.match);

  return `
    <h1 class="hero">Qué plantar<br>con tu clima</h1>
    <p class="mu" style="margin: 10px 0 24px;">
      Según el promedio reciente: ${f1(T)} °C y ${f1(H)} % de humedad.
    </p>

    <div class="grid">
      ${ranking.slice(0, 8).map(r => {
    const isSelected = S.sel.includes(r.p.id);
    return `
          <div class="glass pl ${isSelected ? 'sel' : ''}">
            <div class="top">
              <div>
                <h3>${r.p.n}</h3>
                <span class="mu">${r.match}% de ajuste</span>
              </div>
              <button class="tg" 
                      aria-pressed="${isSelected}" 
                      aria-label="${isSelected ? 'Quitar' : 'Agregar'} ${r.p.n}" 
                      onclick="toggle('${r.p.id}')">
                ${isSelected ? '✓' : '+'}
              </button>
            </div>
            <div class="mb"><i style="left:0; width:${r.match}%"></i></div>
            <p class="mu">
              ${r.match === 100
        ? 'Se da bien con las condiciones actuales.'
        : 'Fuera de rango por ' + [r.dT ? f1(r.dT) + ' °C' : '', r.dH ? f1(r.dH) + ' % de humedad' : ''].filter(Boolean).join(' y ') + '.'
      }
            </p>
          </div>
        `;
  }).join('')}
    </div>
  `;
}

/* ==========================================================================
   9. MODAL FLOTANTE (DETALLE DE PLANTA)
   ========================================================================== */
function abrir(id) {
  const p = P.find(x => x.id === id);
  const isSelected = S.sel.includes(id);

  $('#sheet').innerHTML = `
    <div class="glass card sheet-card" onclick="event.stopPropagation()" role="dialog" aria-label="${p.n}">
      <div class="plant-preview">${planta(id)}</div>
      <h3 class="plant-title">${p.n}</h3>
      
      <div class="mu">Temperatura ${p.t[0]} a ${p.t[1]} °C</div>
      ${mini(p.t[0], p.t[1], 0, 45)}

      <div class="mu">Humedad ${p.h[0]} a ${p.h[1]} %</div>
      ${mini(p.h[0], p.h[1], 10, 100, 'h')}

      <p class="mu">Luz: ${p.luz}. Riego: ${p.riego}.<br>${p.nota}</p>

      <div class="modal-actions">
        <button class="btn flex-1" onclick="cerrar()">Cerrar</button>
        <button class="btn pri flex-1" onclick="toggle('${id}'); cerrar()">${isSelected ? 'Quitar' : 'Agregar'}</button>
      </div>
    </div>
  `;
  $('#sheet').classList.remove('hide');
}

function cerrar() {
  $('#sheet').classList.add('hide');
}

window.addEventListener('keydown', e => {
  if (e.key === 'Escape') cerrar();
});

/* ==========================================================================
   10. CONTROLADOR DE VISTAS Y AUTENTICACIÓN
   ========================================================================== */
const VIEWS = {
  panel: ['Panel', panel],
  plantas: ['Mis plantas', plantas],
  recs: ['Recomendaciones', recs]
};

function draw() {
  S.first = true;
  $('#nav').innerHTML = Object.entries(VIEWS).map(([key, item]) => `
    <button class="${S.view === key ? 'on' : ''}" 
            aria-current="${S.view === key}" 
            onclick="S.view = '${key}'; draw()">
      ${item[0]}
    </button>
  `).join('');

  $('#v').innerHTML = VIEWS[S.view][1]();
  upd();
}

async function entrar() {
  try {
    S.user = await API.login($('#em').value, $('#pw').value);
    save();
    show();
  } catch (err) {
    $('#err').textContent = REAL ? 'Correo o contraseña incorrectos.' : err.message;
  }
}

function salir() {
  if (REAL && fb) fb.au.signOut(fb.auth);
  S.user = null;
  save();
  show();
}

function show() {
  $('#demo').classList.toggle('hide', REAL);
  $('#login').classList.toggle('hide', !!S.user);
  $('#app').classList.toggle('hide', !S.user);
  if (S.user) {
    $('#usr').textContent = S.user.email;
    draw();
  }
}

/* ==========================================================================
   11. CICLO DE VIDA E INICIALIZACIÓN
   ========================================================================== */
if (REAL) {
  // Modo real: la sesión y los datos vienen de Firebase
  S.user = null;
  iniciarFB().then(() => {
    fb.au.onAuthStateChanged(fb.auth, user => {
      S.user = user ? { email: user.email } : null;
      if (user) suscribir(); else subOk = false;
      show();
    });
  });
  // Refresca el panel para detectar "sin datos recientes" aunque no lleguen lecturas
  setInterval(() => { if (S.user) upd(); }, 5000);
} else {
  // Modo demo: lecturas simuladas cada 2 segundos
  for (let i = 0; i < 40; i++) tick();
  setInterval(tick, 2000);
}

// Renderizado según estado inicial de autenticación
show();