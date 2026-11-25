// Cross-platform renderer for generate_flowcharts.ps1 event definitions.
// This keeps the PowerShell file as the single source of flowchart content,
// while allowing generation on machines whose execution policy blocks .ps1.
const fs = require('fs');
const path = require('path');

const directory = __dirname;
const source = fs.readFileSync(path.join(directory, 'generate_flowcharts.ps1'), 'utf8');

function xml(text) {
  return String(text)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&apos;');
}

function parseEvents(variable) {
  const start = source.indexOf(`$${variable} = @(`);
  if (start < 0) throw new Error(`Missing ${variable}`);
  const end = source.indexOf('\n)', start);
  if (end < 0) throw new Error(`Unterminated ${variable}`);
  const block = source.slice(start, end);
  const events = [];
  const pattern = /\(E\s+(\w+)\s+'((?:''|[^'])*)'\s*([^)]*)\)/g;
  let match;
  while ((match = pattern.exec(block)) !== null) {
    const numbers = match[3].trim()
      ? match[3].trim().split(/\s+/).map(Number)
      : [];
    events.push({
      kind: match[1],
      label: match[2].replace(/''/g, "'"),
      from: numbers[0] ?? -1,
      to: numbers[1] ?? -1,
      at: numbers[2] ?? -1
    });
  }
  return events;
}

function render(title, participants, events, outputName) {
  const margin = 110;
  const laneGap = 440;
  const width = margin * 2 + laneGap * (participants.length - 1);
  const top = 165;
  const rowHeight = 78;
  const stageHeight = 58;
  const height = top + 120 + events.reduce(
    (sum, event) => sum + (event.kind === 'stage' ? stageHeight : rowHeight), 0);
  const xs = participants.map((_, index) => margin + laneGap * index);
  const lines = [
    '<?xml version="1.0" encoding="UTF-8"?>',
    `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}">`,
    '  <defs>',
    '    <marker id="arrow" markerWidth="12" markerHeight="9" refX="10" refY="4.5" orient="auto" markerUnits="strokeWidth">',
    '      <path d="M0,0 L10,4.5 L0,9 z" fill="#334155"/>',
    '    </marker>',
    '    <style>',
    '      .title{font:700 34px "Microsoft YaHei","Noto Sans CJK SC",sans-serif;fill:#172033}',
    '      .participant{font:600 20px "Microsoft YaHei","Noto Sans CJK SC",sans-serif;fill:#26324a}',
    '      .label{font:500 17px "Microsoft YaHei","Noto Sans CJK SC",sans-serif;fill:#172033}',
    '      .sub{font:400 14px Consolas,"Microsoft YaHei",sans-serif;fill:#475569}',
    '      .stage{font:700 19px "Microsoft YaHei","Noto Sans CJK SC",sans-serif;fill:#1d4ed8}',
    '      .step{font:700 14px Consolas,sans-serif;fill:#2563eb}',
    '      .life{stroke:#c4b5fd;stroke-width:2;stroke-dasharray:7 7}',
    '      .arrow{stroke:#334155;stroke-width:2;fill:none;marker-end:url(#arrow)}',
    '      .return{stroke:#64748b;stroke-width:2;stroke-dasharray:8 6;fill:none;marker-end:url(#arrow)}',
    '    </style>',
    '  </defs>',
    '  <rect width="100%" height="100%" fill="#ffffff"/>',
    `  <text class="title" x="${width / 2}" y="55" text-anchor="middle">${xml(title)}</text>`
  ];

  participants.forEach((participant, index) => {
    const x = xs[index];
    lines.push(`  <rect x="${x - 135}" y="85" width="270" height="58" rx="9" fill="#f0edff" stroke="#c4b5fd"/>`);
    lines.push(`  <text class="participant" x="${x}" y="121" text-anchor="middle">${xml(participant)}</text>`);
    lines.push(`  <line class="life" x1="${x}" y1="143" x2="${x}" y2="${height - 80}"/>`);
  });

  let y = top;
  let step = 0;
  events.forEach(event => {
    if (event.kind === 'stage') {
      lines.push(`  <rect x="35" y="${y - 28}" width="${width - 70}" height="44" rx="8" fill="#eff6ff" stroke="#93c5fd"/>`);
      lines.push(`  <text class="stage" x="55" y="${y}">${xml(event.label)}</text>`);
      y += stageHeight;
      return;
    }
    step += 1;
    const [main, sub = ''] = event.label.split(/\|(.*)/s);
    lines.push(`  <text class="step" x="36" y="${y + 5}">${String(step).padStart(2, '0')}</text>`);
    if (event.kind === 'self' || event.kind === 'wait') {
      const x = xs[event.at];
      const boxX = Math.max(42, Math.min(x - 190, width - 422));
      const textX = boxX + 190;
      const fill = event.kind === 'wait' ? '#fff7d6' : '#f8fafc';
      const stroke = event.kind === 'wait' ? '#f59e0b' : '#94a3b8';
      lines.push(`  <rect x="${boxX}" y="${y - 28}" width="380" height="58" rx="10" fill="${fill}" stroke="${stroke}" stroke-width="1.5"/>`);
      lines.push(`  <text class="label" x="${textX}" y="${y - 3}" text-anchor="middle">${xml(main)}</text>`);
      if (sub) lines.push(`  <text class="sub" x="${textX}" y="${y + 20}" text-anchor="middle">${xml(sub)}</text>`);
    } else {
      const x1 = xs[event.from];
      const x2 = xs[event.to];
      const cssClass = event.kind === 'return' ? 'return' : 'arrow';
      const mid = (x1 + x2) / 2;
      lines.push(`  <line class="${cssClass}" x1="${x1}" y1="${y}" x2="${x2}" y2="${y}"/>`);
      lines.push(`  <rect x="${mid - 198}" y="${y - 34}" width="396" height="31" rx="5" fill="#ffffff" opacity="0.96"/>`);
      lines.push(`  <text class="label" x="${mid}" y="${y - 12}" text-anchor="middle">${xml(main)}</text>`);
      if (sub) lines.push(`  <text class="sub" x="${mid}" y="${y + 22}" text-anchor="middle">${xml(sub)}</text>`);
    }
    y += rowHeight;
  });

  participants.forEach((participant, index) => {
    const x = xs[index];
    lines.push(`  <rect x="${x - 135}" y="${height - 70}" width="270" height="48" rx="9" fill="#f0edff" stroke="#c4b5fd"/>`);
    lines.push(`  <text class="participant" x="${x}" y="${height - 39}" text-anchor="middle">${xml(participant)}</text>`);
  });
  lines.push('</svg>');
  fs.writeFileSync(path.join(directory, outputName), lines.join('\n') + '\n', 'utf8');
}

render('simple_qt：SLAM 标定正常流程（当前版本）',
       ['操作界面 / QML', '上位机 TcpController', '机械臂 GRR20', 'SLAM 设备'],
       parseEvents('slamEvents'), 'SLAM标定流程.svg');
render('simple_qt：激光标定正常流程（当前单摄像头版本）',
       ['操作界面 / QML', 'LaserCalibrationController', '机械臂', '接收机设备', 'LaserVisionProcessor'],
       parseEvents('laserEvents'), '激光标定流程.svg');
