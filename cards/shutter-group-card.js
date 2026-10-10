/**
 * shutter-group-card
 * Version: v0.6.2 (2026-10-10)
 *
 * A Lovelace custom card for the `shutter_hub` ESPHome component.
 *
 * Layout:
 *   ┌───────────────────────────────────────────────┐
 *   │ <title>                                         │   full-width name
 *   ├───────────────────────────────────────────────┤
 *   │  [icon]   [icon]   ...        [▲] [■] [▼]       │   per-member icon + label, group buttons right
 *   │  45%      Closed                                │
 *   └───────────────────────────────────────────────┘
 *
 * Each member icon is a position-accurate SVG shutter (continuous 0..100%),
 * with an arrow overlay while opening/closing and an hourglass badge while
 * queued. Labels: Open / Closed / NN% / Opening / Closing / Queued Open /
 * Queued Close. The three buttons (close / stop / open) act on the GROUP
 * cover entity via the standard cover services. Icons are display-only.
 *
 * Config:
 *   type: custom:shutter-group-card
 *   title: Bedroom C
 *   group_entity: cover.bedroom_c_group
 *   members:
 *     - cover.bedroom_c_e_shutter_dev          # string form: derives _status/_position
 *     - cover.bedroom_c_w_shutter_dev
 *     # or object form, to spell the entities out explicitly:
 *     - name: Bath
 *       status: sensor.bath_shutter_status
 *       position: sensor.bath_shutter_position
 *   # optional: a sun icon (grey = not through, yellow = shining through) shown
 *   # between the shutter icons and the control buttons. Omit for no sun icon.
 *   sun_through_entity: binary_sensor.bedroom_c_sun_through
 *
 * No build step. Drop this file in HA's config/www/ and register it as a
 * dashboard resource (JavaScript Module), then use type: custom:shutter-group-card.
 */

const CARD_VERSION = "v0.6.2 (2026-10-10)";

// ---------------------------------------------------------------------------
// Appearance configuration. Edit these constants to tune the card's look for
// ALL instances of the card (this is file-level config, not per-card YAML).
// Sizes are in pixels unless noted. Colors accept any CSS color, including HA
// theme variables like "var(--primary-color)".
// ---------------------------------------------------------------------------
const CONFIG = {
  // Shutter (member) icons. A single size (the icon WIDTH in px); the height
  // is derived automatically from the icon's native 48:58 aspect ratio so the
  // shutter always keeps its correct shape.
  shutterIconSize: 24,           // px (width; height auto-scales)
  memberGap: 18,                 // px — horizontal gap between shutter icons
  showLabels: true,              // show the status text under each shutter

  // Sun-through indicator.
  sunIconSize: 24,               // px

  // Control buttons (the tile-style open/stop/close group).
  buttonSize: 32,                // px — button height / touch target
  buttonGroupWidth: 180,         // px — total width of the 3-button group
  buttonIconSize: 24,            // px — the glyph inside each button

  // Typography.
  titleFontSize: "1.1rem",
  labelFontSize: "0.8rem",

  // Colors. Default to HA theme variables so the card matches your theme;
  // override with explicit colors (e.g. "#2196f3") if you prefer.
  shutterColor: "var(--primary-color)",            // slats, headrail, frame
  sunOffColor: "var(--disabled-text-color, #9e9e9e)",   // sun not shining through
  sunOnColor: "var(--state-sun-above-horizon-color, #ffb300)", // sun shining through
  buttonIconColor: "var(--primary-text-color)",
};

const STATUS = {
  OPEN: "open",
  CLOSED: "closed",
  OPENING: "opening",
  CLOSING: "closing",
  QUEUED_OPEN: "queued_open",
  QUEUED_CLOSE: "queued_close",
  PARTIAL: "partial", // "partial:NN"
};

// Capitalized display labels.
function labelFor(status, positionPct) {
  if (!status) {
    // No status entity — fall back to position only.
    if (positionPct == null) return "—";
    if (positionPct >= 99) return "Open";
    if (positionPct <= 1) return "Closed";
    return `${positionPct}%`;
  }
  if (status.startsWith("partial")) {
    const parts = status.split(":");
    const pct = parts.length > 1 ? parseInt(parts[1], 10) : positionPct;
    return Number.isFinite(pct) ? `${pct}%` : "Partial";
  }
  switch (status) {
    case STATUS.OPEN: return "Open";
    case STATUS.CLOSED: return "Closed";
    case STATUS.OPENING: return "Opening";
    case STATUS.CLOSING: return "Closing";
    case STATUS.QUEUED_OPEN: return "Queued Open";
    case STATUS.QUEUED_CLOSE: return "Queued Close";
    default: return status;
  }
}

// Resolve the position percentage (0..100) a member icon should draw.
// Prefer the explicit position sensor; fall back to parsing partial:NN.
function resolvePercent(statusState, positionState) {
  if (positionState != null && positionState !== "unknown" && positionState !== "unavailable") {
    const n = parseFloat(positionState);
    if (Number.isFinite(n)) return Math.max(0, Math.min(100, n));
  }
  if (statusState && statusState.startsWith("partial:")) {
    const n = parseInt(statusState.split(":")[1], 10);
    if (Number.isFinite(n)) return Math.max(0, Math.min(100, n));
  }
  if (statusState === STATUS.OPEN) return 100;
  if (statusState === STATUS.CLOSED) return 0;
  return null;
}

/**
 * Draw a shutter as SVG. percent = how OPEN it is (100 = fully open/retracted,
 * 0 = fully closed/slats covering the window). overlay: "up" | "down" | "queued"
 * | null.
 */
function shutterSvg(percent, overlay) {
  const W = 48, H = 58;

  // Headrail (the box the shutter rolls up into) across the top. Drawn
  // slightly WIDER than the window frame below it, like the MDI shutter icon.
  const railX = 1, railY = 3, railW = W - 2, railH = 8;

  // Window/slat area sits below the headrail, inset so the rail overhangs it.
  const winX = railX + 5;
  const winY = railY + railH;
  const winW = railW - 10;
  const winBottom = H - 4;
  const winH = winBottom - winY;

  // Closed fraction: how much of the window the slats cover (slats hang down
  // from under the headrail). percent = how OPEN it is.
  const closed = percent == null ? 1 : (100 - percent) / 100;
  const coveredH = Math.round(winH * closed);

  const slatCount = 6;
  const slatGap = 1;
  const slatH = Math.max(1, Math.floor((winH - (slatCount - 1) * slatGap) / slatCount));

  // Build slats from just under the headrail downward, only within covered region.
  let slats = "";
  let y = winY;
  for (let i = 0; i < slatCount; i++) {
    const topOffset = y - winY;
    if (topOffset + slatH <= coveredH) {
      slats += `<rect x="${winX}" y="${y}" width="${winW}" height="${slatH}" rx="1" class="sg-slat"/>`;
    } else if (topOffset < coveredH) {
      const h = Math.max(1, coveredH - topOffset);
      slats += `<rect x="${winX}" y="${y}" width="${winW}" height="${h}" rx="1" class="sg-slat"/>`;
    }
    y += slatH + slatGap;
  }

  let ov = "";
  const cx = W / 2;
  const oy = winBottom - 10;
  if (overlay === "up") {
    ov = `<path d="M ${cx} ${oy} l 7 9 l -14 0 Z" class="sg-ov sg-ov-move"/>`;
  } else if (overlay === "down") {
    ov = `<path d="M ${cx} ${oy + 9} l 7 -9 l -14 0 Z" class="sg-ov sg-ov-move"/>`;
  } else if (overlay === "queued") {
    ov = `<g class="sg-ov sg-ov-queued" transform="translate(${cx - 6}, ${oy - 1})">
            <path d="M0 0 h12 l-6 7 Z M0 14 h12 l-6 -7 Z" />
            <rect x="-1" y="-2" width="14" height="2" rx="1"/>
            <rect x="-1" y="14" width="14" height="2" rx="1"/>
          </g>`;
  }

  return `
    <svg viewBox="0 0 ${W} ${H}" class="sg-svg" aria-hidden="true">
      <!-- window frame outline -->
      <rect x="${winX - 2}" y="${winY}" width="${winW + 4}" height="${winH + 2}" rx="2" class="sg-frame"/>
      ${slats}
      <!-- headrail / shutter box across the top -->
      <rect x="${railX}" y="${railY}" width="${railW}" height="${railH}" rx="2.5" class="sg-rail"/>
      ${ov}
    </svg>`;
}

class ShutterGroupCard extends HTMLElement {
  setConfig(config) {
    if (!config || !config.group_entity) {
      throw new Error("shutter-group-card: 'group_entity' is required");
    }
    if (!Array.isArray(config.members) || config.members.length === 0) {
      throw new Error("shutter-group-card: 'members' must be a non-empty list");
    }
    this._config = config;
    this._members = config.members.map((m) => this._normalizeMember(m));
    this._root = null;
  }

  // Accept a bare cover entity string (derive _status/_position) or an object
  // with explicit status/position entity IDs.
  _normalizeMember(m) {
    if (typeof m === "string") {
      // e.g. "cover.bedroom_c_e_shutter_dev" -> base slug "bedroom_c_e_shutter_dev"
      const slug = m.includes(".") ? m.split(".").slice(1).join(".") : m;
      return {
        name: null,
        cover: m.includes(".") ? m : null,
        status: `sensor.${slug}_status`,
        position: `sensor.${slug}_position`,
      };
    }
    return {
      name: m.name || null,
      cover: m.cover || null,
      status: m.status || null,
      position: m.position || null,
    };
  }

  set hass(hass) {
    this._hass = hass;
    this._render();
  }

  getCardSize() {
    return 2;
  }

  _stateOf(entityId) {
    if (!entityId || !this._hass) return null;
    const s = this._hass.states[entityId];
    return s ? s.state : null;
  }

  _callGroup(service) {
    if (!this._hass) return;
    this._hass.callService("cover", service, {
      entity_id: this._config.group_entity,
    });
  }

  // Build the static DOM once. Buttons and their click handlers are created a
  // single time here, so rapid hass updates can never replace an element
  // mid-click (which previously could drop a press). Only the member icons and
  // labels are mutated on each update, in _update().
  _buildOnce() {
    this.attachShadow({ mode: "open" });
    this._root = this.shadowRoot;
    this._root.innerHTML = this._styles();

    this._container = document.createElement("ha-card");
    this._root.appendChild(this._container);

    if (this._config.title) {
      const t = document.createElement("div");
      t.className = "sg-title";
      t.textContent = this._config.title;
      this._container.appendChild(t);
    }

    const body = document.createElement("div");
    body.className = "sg-body";
    this._container.appendChild(body);

    // Members container + one cell per member, created once.
    this._membersEl = document.createElement("div");
    this._membersEl.className = "sg-members";
    body.appendChild(this._membersEl);

    this._cells = this._members.map(() => {
      const cell = document.createElement("div");
      cell.className = "sg-member";
      const icon = document.createElement("div");
      icon.className = "sg-icon";
      const label = document.createElement("div");
      label.className = "sg-label";
      cell.appendChild(icon);
      cell.appendChild(label);
      this._membersEl.appendChild(cell);
      return { cell, icon, label };
    });

    // Optional sun-through indicator, between the member icons and the
    // control buttons. Only created if the card config supplies
    // `sun_through_entity`. Grey when off/unknown, yellow when the sun is
    // shining through (sensor state "on").
    if (this._config.sun_through_entity) {
      this._sunEl = document.createElement("div");
      this._sunEl.className = "sg-sun";
      this._sunEl.innerHTML = this._sunSvg();
      body.appendChild(this._sunEl);
    }

    // Buttons: replicate HA's tile "cover-open-close" feature exactly —
    // an <ha-control-button-group> (horizontal) of <ha-control-button>
    // elements with <ha-svg-icon> children. These are the rounded-rectangle
    // pill buttons from the tile card, not the round ha-icon-buttons.
    const group = document.createElement("ha-control-button-group");
    group.className = "sg-buttons";
    body.appendChild(group);

    // MDI paths HA uses for cover controls: arrow-up (open), stop, arrow-down
    // (close). computeOpenIcon/computeCloseIcon resolve to these for a
    // standard shutter cover.
    const MDI_UP = "M13,20H11V8L5.5,13.5L4.08,12.08L12,4.16L19.92,12.08L18.5,13.5L13,8V20Z";
    const MDI_STOP = "M18,18H6V6H18V18Z";
    const MDI_DOWN = "M11,4H13V16L18.5,10.5L19.92,11.92L12,19.84L4.08,11.92L5.5,10.5L11,16V4Z";

    // Order matches the tile feature: open, stop, close.
    group.appendChild(this._makeControlButton(MDI_UP, "Open", "open_cover"));
    group.appendChild(this._makeControlButton(MDI_STOP, "Stop", "stop_cover"));
    group.appendChild(this._makeControlButton(MDI_DOWN, "Close", "close_cover"));
  }

  // Sun-through indicator SVG (MDI weather-sunny). Color comes from CSS via
  // the sg-sun-on class toggled in _update().
  _sunSvg() {
    const MDI_SUNNY =
      "M12,7A5,5 0 0,1 17,12A5,5 0 0,1 12,17A5,5 0 0,1 7,12A5,5 0 0,1 12,7" +
      "M12,9A3,3 0 0,0 9,12A3,3 0 0,0 12,15A3,3 0 0,0 15,12A3,3 0 0,0 12,9" +
      "M12,2L14.39,5.42C13.65,5.15 12.84,5 12,5C11.16,5 10.35,5.15 9.61,5.42" +
      "L12,2M3.34,7L7.5,6.65C6.9,7.16 6.36,7.78 5.94,8.5C5.5,9.24 5.25,10 5.11,10.79" +
      "L3.34,7M3.36,17L5.12,13.23C5.26,14 5.53,14.78 5.95,15.5C6.37,16.24 6.91,16.86 7.5,17.37" +
      "L3.36,17M20.65,7L18.88,10.79C18.74,10 18.47,9.23 18.05,8.5C17.63,7.78 17.1,7.15 16.5,6.64" +
      "L20.65,7M20.64,17L16.5,17.36C17.09,16.85 17.62,16.22 18.04,15.5C18.46,14.77 18.73,14 18.87,13.21" +
      "L20.64,17M12,22L9.59,18.56C10.33,18.83 11.14,19 12,19C12.82,19 13.63,18.83 14.37,18.56" +
      "L12,22Z";
    return `
      <svg viewBox="0 0 24 24" class="sg-sun-svg" aria-hidden="true">
        <path d="${MDI_SUNNY}"/>
      </svg>`;
  }

  _makeControlButton(path, label, service) {
    const btn = document.createElement("ha-control-button");
    btn.setAttribute("label", label);
    btn.title = label;
    const icon = document.createElement("ha-svg-icon");
    icon.setAttribute("path", path);
    btn.appendChild(icon);
    btn.addEventListener("click", (ev) => {
      ev.stopPropagation();
      this._callGroup(service);
    });
    return btn;
  }

  _render() {
    if (!this._hass) return;
    if (!this._root) this._buildOnce();
    this._update();
  }

  // Mutate only the per-member icon/label; never touch the buttons.
  _update() {
    this._members.forEach((m, i) => {
      const statusState = this._stateOf(m.status);
      const positionState = this._stateOf(m.position);
      const pct = resolvePercent(statusState, positionState);
      const label = labelFor(statusState, pct);

      let overlay = null;
      if (statusState === STATUS.OPENING) overlay = "up";
      else if (statusState === STATUS.CLOSING) overlay = "down";
      else if (statusState === STATUS.QUEUED_OPEN || statusState === STATUS.QUEUED_CLOSE) overlay = "queued";

      const moving = statusState === STATUS.OPENING || statusState === STATUS.CLOSING;
      const queued = statusState === STATUS.QUEUED_OPEN || statusState === STATUS.QUEUED_CLOSE;

      const cell = this._cells[i];
      cell.cell.className = ["sg-member", moving ? "sg-moving" : "", queued ? "sg-queued" : ""]
        .join(" ").trim();
      cell.icon.innerHTML = shutterSvg(pct, overlay);
      cell.label.textContent = label;
    });

    // Sun-through indicator: yellow when the sensor is "on" (sun shining
    // through), grey otherwise (off / unknown / unavailable).
    if (this._sunEl) {
      const on = this._stateOf(this._config.sun_through_entity) === "on";
      this._sunEl.classList.toggle("sg-sun-on", on);
    }
  }

  _styles() {
    const c = CONFIG;
    const labelDisplay = c.showLabels ? "block" : "none";
    // Shutter icon dimensions: one configurable size (width); height derives
    // from the SVG's native 48:58 aspect ratio so the shape never distorts.
    const iconW = c.shutterIconSize;
    const iconH = Math.round(c.shutterIconSize * (58 / 48));
    // Vertically center the sun icon on the shutter ICON (not the whole member
    // column, which also includes the label below). The icon sits at the top
    // of the column, so the sun's center should match the icon's center.
    const sunTopNudge = Math.max(0, Math.round((iconH - c.sunIconSize) / 2));
    return `
      <style>
        ha-card { padding: 12px 16px 14px; }
        .sg-title {
          font-size: ${c.titleFontSize};
          font-weight: 500;
          margin-bottom: 10px;
          color: var(--primary-text-color);
        }
        .sg-body {
          display: flex;
          align-items: flex-start;
          gap: 16px;
        }
        .sg-members {
          display: flex;
          flex-wrap: wrap;
          gap: ${c.memberGap}px;
          flex: 0 1 auto;
        }
        .sg-member {
          display: flex;
          flex-direction: column;
          align-items: center;
          min-width: ${Math.max(iconW + 12, 40)}px;
        }
        .sg-icon { width: ${iconW}px; height: ${iconH}px; }
        .sg-svg { width: 100%; height: 100%; display: block; }
        .sg-frame {
          fill: none;
          stroke: ${c.shutterColor};
          stroke-width: 1.5;
          opacity: 0.85;
        }
        .sg-rail {
          fill: ${c.shutterColor};
        }
        .sg-slat {
          fill: ${c.shutterColor};
          opacity: 0.85;
        }
        .sg-ov { fill: var(--primary-text-color); }
        .sg-ov-move { fill: ${c.shutterColor}; }
        .sg-ov-queued { fill: var(--secondary-text-color); stroke: none; }
        .sg-moving .sg-slat { animation: sg-pulse 1.1s ease-in-out infinite; }
        .sg-queued { opacity: 0.55; }
        @keyframes sg-pulse {
          0%, 100% { opacity: 0.85; }
          50% { opacity: 0.4; }
        }
        .sg-label {
          display: ${labelDisplay};
          margin-top: 4px;
          font-size: ${c.labelFontSize};
          font-weight: 500;
          color: var(--primary-text-color);
          white-space: nowrap;
        }
        /* Sun-through indicator, between members and buttons. Aligned to the
           top of the row and nudged down so its center matches the shutter
           ICON's center (ignoring the label below the icons). */
        .sg-sun {
          flex: 0 0 auto;
          align-self: flex-start;
          margin-top: ${sunTopNudge}px;
          display: inline-flex;
          align-items: center;
          justify-content: center;
        }
        .sg-sun-svg {
          width: ${c.sunIconSize}px;
          height: ${c.sunIconSize}px;
          fill: ${c.sunOffColor};
          transition: fill 0.3s ease;
        }
        .sg-sun.sg-sun-on .sg-sun-svg {
          fill: ${c.sunOnColor};
        }
        /* Tile-style open/stop/close button group, pushed to the right edge so
           the blank space sits AFTER the sun icon:
           shutters - sun - [blank] - controls. */
        .sg-buttons {
          flex: 0 0 auto;
          align-self: center;
          width: ${c.buttonGroupWidth}px;
          height: ${c.buttonSize}px;
          --control-button-group-spacing: 12px;
          --control-button-group-thickness: ${c.buttonSize}px;
          margin-left: auto;
        }
        .sg-buttons ha-control-button {
          --control-button-border-radius: var(--ha-border-radius-6xl, 28px);
          --mdc-icon-size: ${c.buttonIconSize}px;
          --control-button-icon-color: ${c.buttonIconColor};
        }
      </style>`;
  }

  static getConfigElement() {
    return null;
  }

  static getStubConfig() {
    return {
      title: "Shutter Group",
      group_entity: "cover.my_group",
      members: ["cover.member_one", "cover.member_two"],
    };
  }
}

customElements.define("shutter-group-card", ShutterGroupCard);

// Register in the card picker.
window.customCards = window.customCards || [];
window.customCards.push({
  type: "shutter-group-card",
  name: "Shutter Group Card",
  description: "Per-shutter position-accurate icons + group open/stop/close buttons for the shutter_hub component.",
  preview: false,
});

console.info(
  `%c SHUTTER-GROUP-CARD %c ${CARD_VERSION} `,
  "background:#555;color:#fff;border-radius:3px 0 0 3px;padding:2px 4px",
  "background:#1565c0;color:#fff;border-radius:0 3px 3px 0;padding:2px 4px"
);
