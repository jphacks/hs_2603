// かんたん動画エディタの管理画面。画面に出す値は、すべて textContent で入れる（HTML としては扱わない）。

const EFFORT = { auto: '自動', low: '低', medium: '中', high: '高', max: '最大' };
const LEVELS = ['low', 'medium', 'high', 'max'];
const CHOICES = ['auto', ...LEVELS];
const ACTIONS = {
  'user.create': 'ユーザーを追加',
  'user.update': 'ユーザーを変更',
  'user.delete': 'ユーザーを削除',
  'user.reset_password': 'パスワードを作り直し',
  'user.grant': 'トークンを追加',
  'role.create': 'ロールを追加',
  'role.update': 'ロールを変更',
  'role.delete': 'ロールを削除',
  'user.register': '新規登録',
  'user.approve': '登録を承認',
  'settings.update': '設定を変更',
};
/** 新規登録の受け付け方 */
const REGISTRATION = {
  off: '受け付けない',
  approval: '管理者が承認してから使える',
  open: 'すぐに使える',
};

const app = document.getElementById('app');
const toastBox = document.getElementById('toast');
let me = null;

// ---- 部品 ---------------------------------------------------------------------------

/** 要素を作る。props の on〜 はイベント、それ以外は属性。文字の子は textContent として入る */
function h(tag, props = {}, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(props ?? {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2), v);
    else if (k === 'class') el.className = v;
    // style 属性は CSP（style-src 'self'）で止められるので、1 つずつ設定する
    else if (k === 'style') {
      for (const decl of String(v).split(';')) {
        const i = decl.indexOf(':');
        if (i > 0) el.style.setProperty(decl.slice(0, i).trim(), decl.slice(i + 1).trim());
      }
    }
    else if (k === 'value') el.value = v;
    else if (k === 'checked' || k === 'disabled' || k === 'selected' || k === 'required') el[k] = Boolean(v);
    else el.setAttribute(k, v === true ? '' : String(v));
  }
  for (const c of children.flat(Infinity)) {
    if (c === null || c === undefined || c === false) continue;
    el.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return el;
}

/** root に子を足す（h と同じく、配列は平らにして、null・false は飛ばす。append に null を渡すと「null」と出てしまうため） */
function put(root, ...children) {
  for (const c of children.flat(Infinity)) {
    if (c === null || c === undefined || c === false) continue;
    root.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
}

const num = (n) => (n === null || n === undefined ? '—' : Number(n).toLocaleString('ja-JP'));
const tokensOrUnlimited = (n) => (n === null || n === undefined ? '無制限' : `${num(n)}`);
const when = (s) => (s ? new Date(s).toLocaleString('ja-JP', { dateStyle: 'short', timeStyle: 'short' }) : '—');
const thisMonth = () => {
  const d = new Date();
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}`;
};

function toast(message, isError = false) {
  toastBox.textContent = message;
  toastBox.className = `toast show${isError ? ' error' : ''}`;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => (toastBox.className = 'toast'), isError ? 6000 : 3000);
}

async function api(method, url, body) {
  const res = await fetch(url, {
    method,
    credentials: 'same-origin',
    headers: { 'X-Kantan-Admin': '1', ...(body !== undefined ? { 'Content-Type': 'application/json' } : {}) },
    body: body !== undefined ? JSON.stringify(body) : undefined,
  });
  let data = {};
  try {
    data = await res.json();
  } catch {
    // 下で、失敗として扱う
  }
  if (!res.ok) {
    const err = new Error(data.error || `エラー（${res.status}）`);
    err.code = data.code;
    err.status = res.status;
    // ログインが切れたら、ログインの画面に戻す
    if (res.status === 401 && !url.endsWith('/api/auth/login')) {
      me = null;
      showLogin('ログインの期限が切れました。もう一度ログインしてください');
    }
    throw err;
  }
  return data;
}

/** ダイアログを開く。build(dialog, close) で中身を作る。閉じると消える */
function openDialog(build) {
  const dialog = h('dialog');
  const close = () => dialog.close();
  dialog.addEventListener('close', () => dialog.remove());
  dialog.append(build(dialog, close));
  // 読み上げソフト向けに、見出しをダイアログの名前にする
  const heading = dialog.querySelector('h2');
  if (heading) {
    heading.id = `dialog-${Date.now()}`;
    dialog.setAttribute('aria-labelledby', heading.id);
  }
  document.body.append(dialog);
  dialog.showModal();
  dialog.querySelector('input:not([type=hidden]):not([disabled]), select, button')?.focus();
  return dialog;
}

function field(label, control, hint) {
  return h('label', { class: 'field' }, h('span', {}, label), control, hint ? h('div', { class: 'hint' }, hint) : null);
}

function effortSelect(name, current, options, inherit) {
  return h(
    'select',
    { name },
    inherit !== undefined ? h('option', { value: '', selected: current === null }, `ロールのまま（${EFFORT[inherit]}）`) : null,
    options.map((o) => h('option', { value: o, selected: current === o }, EFFORT[o])),
  );
}

/** トークンの上限の入力（数と「無制限」） */
function tokenInput(name, value) {
  const input = h('input', { type: 'number', name, min: '0', step: '1000', value: value ?? '', inputmode: 'numeric', 'aria-label': '月のトークンの上限' });
  const unlimited = h('input', { type: 'checkbox', name: `${name}Unlimited`, checked: value === null });
  const sync = () => (input.disabled = unlimited.checked);
  unlimited.addEventListener('change', sync);
  sync();
  return { el: h('div', { class: 'inline' }, input, h('label', {}, unlimited, '無制限')), read: () => (unlimited.checked ? null : readTokens(input.value)) };
}

function readTokens(text) {
  const n = Number(String(text).replace(/[,\s]/g, ''));
  if (!Number.isSafeInteger(n) || n < 0) throw new Error('トークンは 0 以上の整数で入れてください');
  return n;
}

function usageCell(usage) {
  // 使える量が 0 なら、もう使えないので満タン（赤）にする
  const pct = usage.allowance === null ? 0 : usage.allowance === 0 ? 100 : Math.min(100, (usage.used / usage.allowance) * 100);
  return h(
    'td',
    {},
    `${num(usage.used)} / ${tokensOrUnlimited(usage.allowance)}`,
    usage.granted ? h('div', { class: 'sub' }, `追加 ${num(usage.granted)} を含む`) : null,
    usage.allowance !== null ? h('div', { class: `bar${pct >= 100 ? ' full' : ''}`, role: 'img', 'aria-label': `${Math.round(pct)}% 使用` }, h('div', { style: `width:${pct}%` })) : null,
  );
}

// ---- ログイン・パスワード ------------------------------------------------------------------

function showLogin(message = '') {
  const error = h('p', { class: 'error', role: 'alert' }, message);
  const form = h(
    'form',
    {
      onsubmit: async (e) => {
        e.preventDefault();
        const data = new FormData(form);
        error.textContent = '';
        form.querySelector('button').disabled = true;
        try {
          me = await api('POST', '/api/auth/login', { email: data.get('email'), password: data.get('password'), client: 'admin' });
          route();
        } catch (err) {
          error.textContent = err.message;
        } finally {
          form.querySelector('button').disabled = false;
        }
      },
    },
    field('メールアドレス', h('input', { type: 'email', name: 'email', autocomplete: 'username', required: true })),
    field('パスワード', h('input', { type: 'password', name: 'password', autocomplete: 'current-password', required: true })),
    error,
    h('button', { type: 'submit', class: 'primary' }, 'ログイン'),
  );
  app.replaceChildren(h('main', { class: 'login' }, h('h1', {}, '管理画面'), h('p', { class: 'sub' }, 'かんたん動画エディタの AI サーバー'), form));
  form.querySelector('input').focus();
}

function passwordForm(onDone) {
  const error = h('p', { class: 'error', role: 'alert' });
  const form = h(
    'form',
    {
      onsubmit: async (e) => {
        e.preventDefault();
        const data = new FormData(form);
        error.textContent = '';
        if (data.get('next') !== data.get('again')) {
          error.textContent = '新しいパスワードが 2 回とも同じになっていません';
          return;
        }
        try {
          me = await api('POST', '/api/me/password', { current: data.get('current'), next: data.get('next') });
          form.reset();
          toast('パスワードを変えました');
          onDone?.();
        } catch (err) {
          error.textContent = err.message;
        }
      },
    },
    field('今のパスワード', h('input', { type: 'password', name: 'current', autocomplete: 'current-password', required: true })),
    field('新しいパスワード', h('input', { type: 'password', name: 'next', autocomplete: 'new-password', required: true, minlength: '10' }), '10 文字以上。ほかで使っていないものにしてください'),
    field('新しいパスワード（もう一度）', h('input', { type: 'password', name: 'again', autocomplete: 'new-password', required: true, minlength: '10' })),
    error,
    h('button', { type: 'submit', class: 'primary' }, 'パスワードを変える'),
  );
  return form;
}

function showForcedPassword() {
  app.replaceChildren(
    h('main', { class: 'login' }, h('h1', {}, '新しいパスワードを決めてください'), h('p', { class: 'sub' }, '仮のパスワードでログインしました。続けるには、自分だけのパスワードに変えてください。'), passwordForm(route)),
  );
  app.querySelector('input').focus();
}

// ---- 枠と切り替え ------------------------------------------------------------------------

const PAGES = [
  ['overview', '概要', viewOverview],
  ['users', 'ユーザー', viewUsers],
  ['signup', '新規登録', viewSignup],
  ['roles', 'ロール', viewRoles],
  ['usage', '使用履歴', viewUsage],
  ['audit', '操作の記録', viewAudit],
  ['account', 'アカウント', viewAccount],
];

async function route() {
  if (!me) {
    try {
      me = await api('GET', '/api/me');
    } catch {
      return showLogin();
    }
  }
  if (me.user.mustChangePassword) return showForcedPassword();
  const id = location.hash.replace(/^#\/?/, '') || 'overview';
  const page = PAGES.find((p) => p[0] === id) ?? PAGES[0];
  const content = h('main', { class: 'content', id: 'content' });
  const nav = h(
    'nav',
    { 'aria-label': 'メニュー' },
    PAGES.map(([pid, label]) => h('a', { href: `#/${pid}`, 'aria-current': pid === page[0] ? 'page' : null }, label)),
  );
  const logout = h('button', {
    type: 'button',
    onclick: async () => {
      await api('POST', '/api/auth/logout').catch(() => {});
      me = null;
      showLogin('ログアウトしました');
    },
  }, 'ログアウト');
  app.replaceChildren(
    h(
      'div',
      { class: 'shell' },
      h('aside', { class: 'side' }, h('div', { class: 'brand' }, h('h1', {}, '管理画面'), h('small', {}, 'かんたん動画エディタ')), nav, h('div', { class: 'me' }, me.user.email, h('br'), logout)),
      content,
    ),
  );
  document.title = `${page[1]} - 管理画面`;
  try {
    await page[2](content);
  } catch (err) {
    if (err.status !== 401) content.append(h('p', { class: 'error', role: 'alert' }, err.message));
  }
}

window.addEventListener('hashchange', route);

// ---- 概要 ----------------------------------------------------------------------------

async function viewOverview(root, month = thisMonth()) {
  const data = await api('GET', `/api/admin/overview?month=${encodeURIComponent(month)}`);
  const picker = h('input', { type: 'month', value: data.month, 'aria-label': '月', onchange: (e) => e.target.value && rerender(() => viewOverview(root, e.target.value)) });
  const rerender = async (fn) => {
    root.replaceChildren();
    await fn();
  };
  const notices = [];
  if (!data.server.bedrockConfigured) notices.push('サーバーに Bedrock の API キー（AWS_BEARER_TOKEN_BEDROCK）がありません。AI は動きません。server/.env に書いて、サーバーを起動し直してください。');
  if (!data.server.modelSupportsEffort) notices.push(`モデル ${data.server.model} は考える深さを変えられません。深さの設定は使われず、モデルの標準で動きます。`);
  const pending = Number(data.accounts.pending ?? 0);
  if (pending > 0) notices.push(h('span', {}, `承認待ちの新規登録が ${num(pending)} 人あります。`, h('a', { href: '#/signup' }, '新規登録の画面で承認する')));
  // グラフは月の 1 日から末日まで並べる（使わなかった日も 0 として並べる）
  const [year, mon] = data.month.split('-').map(Number);
  const byDay = new Map(data.daily.map((d) => [d.day, d]));
  const days = Array.from({ length: new Date(year, mon, 0).getDate() }, (_, i) => {
    const day = `${data.month}-${String(i + 1).padStart(2, '0')}`;
    return byDay.get(day) ?? { day, tokens: 0, requests: 0 };
  });
  const max = Math.max(1, ...days.map((d) => d.tokens));
  put(
    root,
    h('div', { class: 'toolbar' }, h('h2', { class: 'grow' }, '概要'), picker),
    notices.map((n) => h('div', { class: 'notice', role: 'note' }, n)),
    h(
      'div',
      { class: 'cards' },
      card('使ったトークン', num(data.totals.total), `入力 ${num(data.totals.input)}（うちキャッシュ ${num(data.totals.cached)}）・出力 ${num(data.totals.output)}`),
      card('AI に送った回数', num(data.totals.requests), data.totals.errors ? `失敗 ${num(data.totals.errors)} 回` : '失敗なし'),
      card('使った人', `${num(data.totals.users)} 人`, `アカウント ${num(data.accounts.users)}（止めている ${num(data.accounts.disabled)}）`),
      card('モデル', data.server.model, `${data.server.region}・${data.server.timezone}`, 'small'),
    ),
    h(
      'div',
      { class: 'box' },
      h('h2', {}, '日ごとのトークン'),
      data.daily.length
        ? [
            h(
              'div',
              { class: 'chart', role: 'img', 'aria-label': `日ごとのトークンのグラフ（いちばん多い日は ${num(max)} トークン）` },
              days.map((d) =>
                h('div', { class: `col${d.tokens ? '' : ' zero'}`, style: `height:${d.tokens ? Math.max(2, (d.tokens / max) * 100) : 0}%`, title: `${d.day}: ${num(d.tokens)} トークン・${num(d.requests)} 回` }),
              ),
            ),
            h('div', { class: 'chart-axis' }, h('span', {}, days[0].day), h('span', {}, days[days.length - 1].day)),
          ]
        : h('p', { class: 'empty' }, 'この月は、まだ使われていません'),
    ),
    h('h2', {}, 'たくさん使った人'),
    table(
      ['ユーザー', ['トークン', 'num'], ['回数', 'num']],
      data.topUsers.map((u) => [cellUser(u.displayName, u.email), [num(u.tokens), 'num'], [num(u.requests), 'num']]),
      'この月は、まだ使われていません',
    ),
  );
}

function card(label, value, sub, size) {
  return h('div', { class: 'card' }, h('div', { class: 'label' }, label), h('div', { class: `value${size ? ` ${size}` : ''}` }, value), sub ? h('div', { class: 'sub' }, sub) : null);
}

function cellUser(name, email) {
  return h('td', {}, name || email, name ? h('div', { class: 'sub' }, email) : null);
}

/** 表を作る。columns は 見出し か [見出し, クラス]、rows のセルは 文字・要素・td・[文字, クラス] */
function table(columns, rows, emptyText) {
  if (!rows.length) return h('div', { class: 'table-wrap' }, h('p', { class: 'empty' }, emptyText));
  const head = h('tr', {}, columns.map((c) => (Array.isArray(c) ? h('th', { class: c[1], scope: 'col' }, c[0]) : h('th', { scope: 'col' }, c))));
  const body = rows.map((cells) =>
    h(
      'tr',
      {},
      cells.map((c) => {
        if (c instanceof HTMLTableCellElement) return c;
        if (Array.isArray(c)) return h('td', { class: c[1] }, c[0]);
        return h('td', {}, c);
      }),
    ),
  );
  return h('div', { class: 'table-wrap' }, h('table', {}, h('thead', {}, head), h('tbody', {}, body)));
}

// ---- ユーザー --------------------------------------------------------------------------

async function viewUsers(root) {
  const [{ users, month }, { roles }] = await Promise.all([api('GET', '/api/admin/users'), api('GET', '/api/admin/roles')]);
  const search = h('input', { type: 'search', placeholder: 'メールアドレス・名前で探す', 'aria-label': 'ユーザーを探す' });
  const list = h('div');
  const draw = () => {
    const q = search.value.trim().toLowerCase();
    const shown = users.filter((u) => !q || u.email.includes(q) || u.displayName.toLowerCase().includes(q));
    list.replaceChildren(
      table(
        ['ユーザー', 'ロール', '状態', `今月（${month}）使った / 使える量`, '考える深さ', '最後のログイン', ''],
        shown.map((u) => [
          cellUser(u.displayName, u.email),
          h('td', {}, h('span', { class: `tag${u.isAdmin ? ' admin' : ''}` }, u.roleName)),
          h(
            'td',
            {},
            u.pending
              ? h('span', { class: 'tag warn' }, '承認待ち')
              : u.disabled
                ? h('span', { class: 'tag off' }, '止めている')
                : !u.policy.canUseAi
                  ? h('span', { class: 'tag warn' }, 'AI なし')
                  : h('span', { class: 'tag ok' }, '使える'),
            u.mustChangePassword ? h('div', { class: 'sub' }, '仮のパスワード') : null,
            u.selfRegistered ? h('div', { class: 'sub' }, '自分で登録') : null,
          ),
          usageCell(u.usage),
          h('td', {}, `上限 ${EFFORT[u.policy.maxEffort]}`, h('div', { class: 'sub' }, `最初は ${EFFORT[u.policy.defaultEffort]}${u.maxEffort || u.defaultEffort ? '（この人だけ）' : ''}`)),
          when(u.lastLoginAt),
          h(
            'td',
            {},
            u.pending ? h('button', { type: 'button', class: 'link', onclick: () => approveUser(u) }, '承認') : null,
            h('button', { type: 'button', class: 'link', onclick: () => editUser(u, roles) }, '編集'),
          ),
        ]),
        '該当するユーザーはいません',
      ),
    );
  };
  search.addEventListener('input', draw);
  put(
    root,
    h('div', { class: 'toolbar' }, h('h2', { class: 'grow' }, 'ユーザー'), search, h('button', { type: 'button', class: 'primary', onclick: () => editUser(null, roles) }, '＋ ユーザーを追加')),
    list,
  );
  draw();
}

function showTemporaryPassword(email, password) {
  openDialog((dialog, close) => {
    const input = h('input', { type: 'text', value: password, readonly: true, 'aria-label': '仮のパスワード' });
    return h(
      'form',
      { method: 'dialog' },
      h('h2', {}, '仮のパスワード'),
      h('p', {}, `${email} の仮のパスワードです。本人に伝えてください。最初にログインしたときに、新しいパスワードを決めてもらいます。`),
      h('div', { class: 'secret' }, input, h('button', {
        type: 'button',
        onclick: async () => {
          await navigator.clipboard.writeText(password).then(() => toast('コピーしました'), () => input.select());
        },
      }, 'コピー')),
      h('p', { class: 'hint' }, 'この画面を閉じると、もう表示できません（忘れたら作り直してください）。'),
      h('div', { class: 'actions' }, h('button', { type: 'submit', class: 'primary', onclick: close }, '閉じる')),
    );
  });
}

function editUser(user, roles) {
  const isNew = !user;
  // 自分自身: 止める・削除・パスワードの作り直しは出さない（サーバーでも断る。自分のパスワードは「アカウント」で変える）
  const self = !isNew && me?.user.id === user.id;
  openDialog((dialog, close) => {
    const error = h('p', { class: 'error', role: 'alert' });
    const roleSelect = h('select', { name: 'roleId', required: true }, roles.map((r) => h('option', { value: r.id, selected: user ? user.roleId === r.id : !r.isAdmin && r.name === '一般' }, `${r.name}${r.isAdmin ? '（管理者）' : ''}`)));
    const roleOf = () => roles.find((r) => String(r.id) === roleSelect.value) ?? roles[0];
    const roleTokens = h('span');
    const custom = h('input', { type: 'radio', name: 'tokensMode', value: 'custom', checked: user?.tokensCustom });
    const inherit = h('input', { type: 'radio', name: 'tokensMode', value: 'role', checked: !user?.tokensCustom });
    const tokens = tokenInput('monthlyTokens', user?.tokensCustom ? user.monthlyTokens : roleOf().monthlyTokens);
    const maxSel = h('span');
    const defSel = h('span');
    const drawRole = () => {
      const r = roleOf();
      roleTokens.textContent = `ロールのまま（${tokensOrUnlimited(r.monthlyTokens)} / 月）`;
      maxSel.replaceChildren(effortSelect('maxEffort', user?.maxEffort ?? null, LEVELS, r.maxEffort));
      defSel.replaceChildren(effortSelect('defaultEffort', user?.defaultEffort ?? null, CHOICES, r.defaultEffort));
      tokens.el.style.display = custom.checked ? '' : 'none';
    };
    roleSelect.addEventListener('change', drawRole);
    custom.addEventListener('change', drawRole);
    inherit.addEventListener('change', drawRole);
    const disabled = h('input', { type: 'checkbox', name: 'disabled', checked: user?.disabled });
    const form = h(
      'form',
      {
        onsubmit: async (e) => {
          e.preventDefault();
          error.textContent = '';
          const data = new FormData(form);
          try {
            const body = {
              email: data.get('email'),
              displayName: data.get('displayName'),
              roleId: Number(data.get('roleId')),
              tokensCustom: custom.checked,
              maxEffort: data.get('maxEffort') || null,
              defaultEffort: data.get('defaultEffort') || null,
            };
            if (custom.checked) body.monthlyTokens = tokens.read();
            if (!isNew && !self) body.disabled = disabled.checked;
            if (isNew) {
              const res = await api('POST', '/api/admin/users', body);
              close();
              toast('ユーザーを追加しました');
              showTemporaryPassword(res.user.email, res.temporaryPassword);
            } else {
              await api('PATCH', `/api/admin/users/${user.id}`, body);
              close();
              toast('保存しました');
            }
            route();
          } catch (err) {
            error.textContent = err.message;
          }
        },
      },
      h('h2', {}, isNew ? 'ユーザーを追加' : 'ユーザーを編集'),
      field('メールアドレス', h('input', { type: 'email', name: 'email', value: user?.email ?? '', required: true, autocomplete: 'off' }), isNew ? 'ログインに使います。仮のパスワードは自動で作ります' : null),
      field('名前', h('input', { type: 'text', name: 'displayName', value: user?.displayName ?? '', autocomplete: 'off' })),
      field('ロール', roleSelect),
      h('fieldset', { class: 'field' }, h('legend', {}, '月のトークンの上限'), h('div', { class: 'inline' }, h('label', {}, inherit, roleTokens), h('label', {}, custom, 'この人だけ決める')), h('div', { style: 'margin-top:6px' }, tokens.el)),
      field('考える深さの上限', maxSel, 'アプリで選べる深さは、ここまでになります'),
      field('アプリで最初に選ばれる深さ', defSel),
      isNew || self ? null : h('div', { class: 'field inline' }, h('label', {}, disabled, 'アカウントを止める（ログインも AI も使えなくなります）')),
      error,
      h(
        'div',
        { class: 'actions' },
        isNew
          ? null
          : h(
              'div',
              { class: 'left' },
              h('button', {
                type: 'button',
                onclick: () => {
                  close();
                  grantTokens(user);
                },
              }, 'トークンを追加'),
              self ? null : h('button', {
                type: 'button',
                onclick: async () => {
                  if (!confirm(`${user.email} のパスワードを作り直しますか？ 今のパスワードは使えなくなり、ログイン中のアプリもログアウトします。`)) return;
                  try {
                    const res = await api('POST', `/api/admin/users/${user.id}/password`, {});
                    close();
                    showTemporaryPassword(user.email, res.temporaryPassword);
                    route();
                  } catch (err) {
                    error.textContent = err.message;
                  }
                },
              }, 'パスワードを作り直す'),
              self ? null : h('button', {
                type: 'button',
                class: 'danger',
                onclick: async () => {
                  if (!confirm(`${user.email} を削除しますか？ 元に戻せません（使用履歴は残ります）。止めるだけなら「アカウントを止める」を使ってください。`)) return;
                  try {
                    await api('DELETE', `/api/admin/users/${user.id}`);
                    close();
                    toast('削除しました');
                    route();
                  } catch (err) {
                    error.textContent = err.message;
                  }
                },
              }, '削除'),
            ),
        h('button', { type: 'button', onclick: close }, 'キャンセル'),
        h('button', { type: 'submit', class: 'primary' }, isNew ? '追加' : '保存'),
      ),
    );
    drawRole();
    return form;
  });
}

function grantTokens(user) {
  openDialog((dialog, close) => {
    const error = h('p', { class: 'error', role: 'alert' });
    const history = h('div', {}, h('p', { class: 'hint' }, '読み込んでいます…'));
    api('GET', `/api/admin/users/${user.id}/grants`).then(({ grants }) => {
      history.replaceChildren(
        grants.length
          ? table(['月', ['トークン', 'num'], 'メモ', '追加した人'], grants.slice(0, 10).map((g) => [g.month, [num(g.tokens), 'num'], g.note, g.grantedBy]), '')
          : h('p', { class: 'hint' }, 'まだ追加していません'),
      );
    }, (err) => history.replaceChildren(h('p', { class: 'error' }, err.message)));
    const form = h(
      'form',
      {
        onsubmit: async (e) => {
          e.preventDefault();
          const data = new FormData(form);
          error.textContent = '';
          try {
            const n = Number(String(data.get('tokens')).replace(/[,\s]/g, ''));
            if (!Number.isSafeInteger(n) || n === 0) throw new Error('トークンを整数で入れてください（減らすときはマイナス）');
            await api('POST', `/api/admin/users/${user.id}/grants`, { tokens: n, note: data.get('note'), month: data.get('month') });
            close();
            toast('トークンを追加しました');
            route();
          } catch (err) {
            error.textContent = err.message;
          }
        },
      },
      h('h2', {}, `トークンを追加（${user.displayName || user.email}）`),
      h('p', {}, `今月: ${num(user.usage.used)} / ${tokensOrUnlimited(user.usage.allowance)} を使っています。追加はその月だけ使えます。`),
      field('追加するトークン', h('input', { type: 'number', name: 'tokens', step: '1000', required: true, inputmode: 'numeric' }), 'マイナスにすると減らせます'),
      field('月', h('input', { type: 'month', name: 'month', value: thisMonth(), required: true })),
      field('メモ', h('input', { type: 'text', name: 'note', maxlength: '200', placeholder: '例: 発表の準備のため' })),
      h('h3', {}, '最近の追加'),
      history,
      error,
      h('div', { class: 'actions' }, h('button', { type: 'button', onclick: close }, 'キャンセル'), h('button', { type: 'submit', class: 'primary' }, '追加')),
    );
    return form;
  });
}

// ---- ロール --------------------------------------------------------------------------

// ---- 新規登録 ------------------------------------------------------------------------

async function approveUser(u) {
  try {
    await api('POST', `/api/admin/users/${u.id}/approve`, {});
    toast(`${u.displayName || u.email} を承認しました`);
    route();
  } catch (err) {
    toast(err.message, true);
  }
}

async function rejectUser(u) {
  if (!confirm(`${u.email} の登録を断りますか？ このアカウントは削除されます。`)) return;
  try {
    await api('DELETE', `/api/admin/users/${u.id}`);
    toast('登録を断りました');
    route();
  } catch (err) {
    toast(err.message, true);
  }
}

async function viewSignup(root) {
  const [{ registration }, { roles }, { users }] = await Promise.all([api('GET', '/api/admin/settings'), api('GET', '/api/admin/roles'), api('GET', '/api/admin/users')]);
  const error = h('p', { class: 'error', role: 'alert' });
  // 自分で登録した人は、管理画面を使えないロールにだけ入れる
  const choices = roles.filter((r) => !r.isAdmin);
  const roleSelect = h(
    'select',
    { name: 'roleId', required: true },
    choices.map((r) => h('option', { value: r.id, selected: r.id === registration.roleId }, `${r.name}（月 ${tokensOrUnlimited(r.monthlyTokens)} トークン・深さは${EFFORT[r.maxEffort]}まで）`)),
  );
  const hints = {
    approval: '登録した人は、下の「承認待ち」で承認するまでログインできません（おすすめ）',
    open: '登録すると、すぐに AI を使えます。家の外からもつながるサーバーでは、知らない人も使えてしまうので気をつけてください',
    off: 'アプリの「新規登録」は使えません。アカウントは「ユーザー」の画面で作ります',
  };
  const form = h(
    'form',
    {
      onsubmit: async (e) => {
        e.preventDefault();
        error.textContent = '';
        const data = new FormData(form);
        try {
          if (!choices.length) throw new Error('管理画面を使えないロールがありません。先に「ロール」の画面で作ってください');
          await api('PATCH', '/api/admin/settings', { registration: { mode: data.get('mode'), roleId: Number(data.get('roleId')) } });
          toast('保存しました');
          route();
        } catch (err) {
          error.textContent = err.message;
        }
      },
    },
    h(
      'fieldset',
      { class: 'field' },
      h('legend', {}, '受け付け方'),
      ['approval', 'open', 'off'].map((m) =>
        h(
          'label',
          { class: 'choice' },
          h('input', { type: 'radio', name: 'mode', value: m, checked: registration.mode === m }),
          h('span', {}, h('span', {}, REGISTRATION[m]), h('span', { class: 'hint' }, hints[m])),
        ),
      ),
    ),
    field('登録した人のロール', roleSelect, '月のトークンの上限や考える深さは、このロールのものになります（あとから、人ごとに変えることもできます）'),
    error,
    h('button', { type: 'submit', class: 'primary' }, '保存'),
  );
  const pending = users.filter((u) => u.pending);
  put(
    root,
    h('div', { class: 'toolbar' }, h('h2', { class: 'grow' }, '新規登録')),
    h('p', { class: 'hint' }, 'アプリのログインの画面の「新規登録」から、使う人が自分でアカウントを作れます（メールアドレスとパスワード）。'),
    h('div', { class: 'box narrow' }, h('h2', {}, '新規登録の受け付け'), form),
    h('h2', {}, `承認待ち（${num(pending.length)} 人）`),
    table(
      ['ユーザー', '登録した日時', 'ロール', ''],
      pending.map((u) => [
        cellUser(u.displayName, u.email),
        when(u.createdAt),
        h('td', {}, h('span', { class: 'tag' }, u.roleName)),
        h(
          'td',
          {},
          h('button', { type: 'button', class: 'link', onclick: () => approveUser(u) }, '承認'),
          h('button', { type: 'button', class: 'link danger', onclick: () => rejectUser(u) }, '断る'),
        ),
      ]),
      '承認待ちの人はいません',
    ),
  );
}

async function viewRoles(root) {
  const { roles } = await api('GET', '/api/admin/roles');
  put(
    root,
    h('div', { class: 'toolbar' }, h('h2', { class: 'grow' }, 'ロール'), h('button', { type: 'button', class: 'primary', onclick: () => editRole(null) }, '＋ ロールを追加')),
    h('p', { class: 'hint' }, 'ロールで決めた上限と深さが、そのロールの人の標準になります（ユーザーの画面で、人ごとに変えることもできます）。'),
    table(
      ['名前', '管理画面', 'AI', ['月のトークンの上限', 'num'], '考える深さの上限', '最初の深さ', ['人数', 'num'], ''],
      roles.map((r) => [
        h('td', {}, h('span', { class: `tag${r.isAdmin ? ' admin' : ''}` }, r.name)),
        r.isAdmin ? '使える' : '—',
        r.canUseAi ? '使える' : '使えない',
        [tokensOrUnlimited(r.monthlyTokens), 'num'],
        EFFORT[r.maxEffort],
        EFFORT[r.defaultEffort],
        [num(r.users), 'num'],
        h('td', {}, h('button', { type: 'button', class: 'link', onclick: () => editRole(r) }, '編集')),
      ]),
      'ロールがありません',
    ),
  );
}

function editRole(role) {
  const isNew = !role;
  openDialog((dialog, close) => {
    const error = h('p', { class: 'error', role: 'alert' });
    const tokens = tokenInput('monthlyTokens', role ? role.monthlyTokens : 1000000);
    const form = h(
      'form',
      {
        onsubmit: async (e) => {
          e.preventDefault();
          const data = new FormData(form);
          error.textContent = '';
          try {
            const body = {
              name: data.get('name'),
              isAdmin: data.get('isAdmin') === 'on',
              canUseAi: data.get('canUseAi') === 'on',
              monthlyTokens: tokens.read(),
              maxEffort: data.get('maxEffort'),
              defaultEffort: data.get('defaultEffort'),
            };
            await api(isNew ? 'POST' : 'PATCH', isNew ? '/api/admin/roles' : `/api/admin/roles/${role.id}`, body);
            close();
            toast(isNew ? 'ロールを追加しました' : '保存しました');
            route();
          } catch (err) {
            error.textContent = err.message;
          }
        },
      },
      h('h2', {}, isNew ? 'ロールを追加' : 'ロールを編集'),
      field('名前', h('input', { type: 'text', name: 'name', value: role?.name ?? '', required: true, maxlength: '50' })),
      h('div', { class: 'field inline' }, h('label', {}, h('input', { type: 'checkbox', name: 'isAdmin', checked: role?.isAdmin }), '管理画面を使える（管理者）')),
      h('div', { class: 'field inline' }, h('label', {}, h('input', { type: 'checkbox', name: 'canUseAi', checked: role ? role.canUseAi : true }), 'AI アシスタントを使える')),
      h('div', { class: 'field' }, h('span', {}, '月のトークンの上限（1 人あたり。入力と出力の合計）'), tokens.el),
      field('考える深さの上限', effortSelect('maxEffort', role?.maxEffort ?? 'high', LEVELS)),
      field('アプリで最初に選ばれる深さ', effortSelect('defaultEffort', role?.defaultEffort ?? 'auto', CHOICES), '上限より深いものを選んだときは、上限になります'),
      error,
      h(
        'div',
        { class: 'actions' },
        isNew
          ? null
          : h('div', { class: 'left' }, h('button', {
              type: 'button',
              class: 'danger',
              onclick: async () => {
                if (!confirm(`ロール「${role.name}」を削除しますか？`)) return;
                try {
                  await api('DELETE', `/api/admin/roles/${role.id}`);
                  close();
                  toast('削除しました');
                  route();
                } catch (err) {
                  error.textContent = err.message;
                }
              },
            }, '削除')),
        h('button', { type: 'button', onclick: close }, 'キャンセル'),
        h('button', { type: 'submit', class: 'primary' }, isNew ? '追加' : '保存'),
      ),
    );
    return form;
  });
}

// ---- 使用履歴 ------------------------------------------------------------------------

async function viewUsage(root, state = { month: thisMonth(), userId: '', offset: 0 }) {
  const params = new URLSearchParams({ month: state.month, offset: String(state.offset), limit: '50' });
  if (state.userId) params.set('userId', state.userId);
  const [data, { users }] = await Promise.all([api('GET', `/api/admin/usage?${params}`), api('GET', '/api/admin/users')]);
  const redraw = (next) => {
    root.replaceChildren();
    viewUsage(root, { ...state, ...next }).catch((err) => root.append(h('p', { class: 'error' }, err.message)));
  };
  const csv = new URLSearchParams({ month: state.month });
  if (state.userId) csv.set('userId', state.userId);
  const total = Number(data.sums.count);
  put(
    root,
    h(
      'div',
      { class: 'toolbar' },
      h('h2', { class: 'grow' }, '使用履歴'),
      h('input', { type: 'month', value: data.month, 'aria-label': '月', onchange: (e) => e.target.value && redraw({ month: e.target.value, offset: 0 }) }),
      h('select', { 'aria-label': 'ユーザー', onchange: (e) => redraw({ userId: e.target.value, offset: 0 }) }, h('option', { value: '' }, 'すべての人'), users.map((u) => h('option', { value: u.id, selected: String(u.id) === state.userId }, u.email))),
      h('a', { class: 'button', href: `/api/admin/usage.csv?${csv}`, download: '' }, 'CSV で保存'),
    ),
    h(
      'div',
      { class: 'cards' },
      card('回数', num(total)),
      card('入力', num(data.sums.input), `うちキャッシュ ${num(data.sums.cached)}`),
      card('出力', num(data.sums.output), '考えた分を含む'),
    ),
    table(
      ['日時', 'ユーザー', '考える深さ', ['入力', 'num'], ['出力', 'num'], ['時間', 'num'], '結果'],
      data.rows.map((r) => [
        when(r.createdAt),
        r.email,
        h('td', {}, r.effort ? EFFORT[r.effort] : 'モデルの標準', r.effortChoice && r.effortChoice !== r.effort ? h('div', { class: 'sub' }, `選んだもの: ${EFFORT[r.effortChoice] ?? r.effortChoice}`) : null),
        [num(r.input), 'num'],
        [num(r.output), 'num'],
        [`${(r.durationMs / 1000).toFixed(1)} 秒`, 'num'],
        h('td', { class: r.ok ? '' : 'wrap' }, r.ok ? h('span', { class: 'tag ok' }, '成功') : [h('span', { class: 'tag off' }, '失敗'), h('div', { class: 'sub' }, r.error)]),
      ]),
      'この条件では、まだ使われていません',
    ),
    total > data.limit
      ? h(
          'div',
          { class: 'pager' },
          `${num(state.offset + 1)}〜${num(Math.min(total, state.offset + data.limit))} 件目 / ${num(total)} 件`,
          h('button', { type: 'button', disabled: state.offset === 0, onclick: () => redraw({ offset: Math.max(0, state.offset - data.limit) }) }, '前へ'),
          h('button', { type: 'button', disabled: state.offset + data.limit >= total, onclick: () => redraw({ offset: state.offset + data.limit }) }, '次へ'),
        )
      : null,
  );
}

// ---- 操作の記録 ------------------------------------------------------------------------

/** 操作の記録の項目名（データベースの列の名前 → 画面に出す名前） */
const FIELD_NAMES = {
  email: 'メールアドレス',
  display_name: '名前',
  name: '名前',
  role: 'ロール',
  disabled: '止める',
  tokens_custom: '上限をこの人だけ決める',
  monthly_tokens: '月の上限',
  max_effort: '深さの上限',
  default_effort: '最初の深さ',
  is_admin: '管理画面',
  can_use_ai: 'AI',
  tokens: 'トークン',
  month: '月',
  note: 'メモ',
  registration: '受け付け方',
  pending: '承認待ち',
};

function fieldValue(key, value) {
  if (value === null) return key === 'monthly_tokens' ? '無制限' : key.endsWith('_effort') ? 'ロールのまま' : '—';
  if (typeof value === 'boolean') return value ? 'はい' : 'いいえ';
  if (key === 'registration') return REGISTRATION[value] ?? String(value);
  if (key.endsWith('_effort')) return EFFORT[value] ?? String(value);
  if (typeof value === 'number') return num(value);
  return String(value);
}

async function viewAudit(root) {
  const { rows } = await api('GET', '/api/admin/audit');
  const detail = (d) =>
    Object.entries(d ?? {})
      .map(([k, v]) => `${FIELD_NAMES[k] ?? k}: ${fieldValue(k, v)}`)
      .join('、');
  put(
    root,
    h('div', { class: 'toolbar' }, h('h2', { class: 'grow' }, '操作の記録'), h('span', { class: 'hint' }, '管理画面での変更（新しい順に 200 件）')),
    table(
      ['日時', '管理者', '操作', '対象', '内容'],
      // 新規登録は管理者の操作ではないので、管理者の欄は「本人」
      rows.map((r) => [when(r.createdAt), r.adminEmail || '（本人）', ACTIONS[r.action] ?? r.action, r.target, h('td', { class: 'wrap' }, detail(r.detail))]),
      'まだ記録はありません',
    ),
  );
}

// ---- アカウント ------------------------------------------------------------------------

async function viewAccount(root) {
  put(
    root,
    h('h2', {}, 'アカウント'),
    h('div', { class: 'box' }, h('p', {}, `${me.user.displayName || me.user.email}（${me.user.role}）`), h('p', { class: 'hint' }, me.user.email)),
    h('div', { class: 'box', style: 'max-width:420px' }, h('h2', {}, 'パスワードを変える'), passwordForm()),
  );
}

route();
