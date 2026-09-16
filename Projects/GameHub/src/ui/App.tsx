/**
 * @file App.tsx
 * @brief プロジェクト一覧・テンプレート・設定をまとめる GameHub のルート UI。
 * @author Hasegawa Jin
 * @date 2026/09/02
 */

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { BootstrapData, CreateProjectRequest, HubSettings, HubTheme, ProjectEntry, SdkBuildConfiguration, TemplateInfo } from '../shared/contracts';
import { deriveProjectIdentifiers, isValidProjectIdentifiers } from '../shared/contracts';
import { Icon } from './Icon';
import { Logo } from './Logo';

type Page = 'projects' | 'templates' | 'settings';
type ViewMode = 'list' | 'grid';
type SortKey = 'recent' | 'name' | 'status';
type StatusFilter = 'all' | 'ready' | 'issues';
type ProjectStatus = 'pending' | 'ready' | 'warning' | 'error';
type Severity = 'error' | 'warning' | 'info';

interface Diagnostic {
  severity: Severity;
  label: string;
  detail: string;
}

interface InspectedProject {
  project: ProjectEntry;
  diagnostics: Diagnostic[];
  status: ProjectStatus;
}

const STATUS_LABEL: Record<ProjectStatus, string> = { pending: '検証中', ready: '正常', warning: '要確認', error: 'エラー' };
const STATUS_ORDER: Record<ProjectStatus, number> = { error: 0, warning: 1, pending: 2, ready: 3 };
const THEME_LABEL: Record<HubTheme, string> = { modern: 'Modern', dark: 'Dark', light: 'Light', system: 'System' };
const CONFIGURATIONS: SdkBuildConfiguration[] = ['Debug', 'Development', 'Release'];

function formatTimestamp(value: string): { label: string; title: string } {
  if (!value) return { label: '未起動', title: '一度も開かれていません' };
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return { label: value, title: value };
  const absolute = new Intl.DateTimeFormat('ja-JP', { dateStyle: 'medium', timeStyle: 'short' }).format(date);
  const minutes = Math.floor((Date.now() - date.getTime()) / 60_000);
  if (minutes < 1) return { label: 'たった今', title: absolute };
  if (minutes < 60) return { label: `${minutes} 分前`, title: absolute };
  if (minutes < 1_440) return { label: `${Math.floor(minutes / 60)} 時間前`, title: absolute };
  if (minutes < 43_200) return { label: `${Math.floor(minutes / 1_440)} 日前`, title: absolute };
  return { label: absolute.split(' ')[0] ?? absolute, title: absolute };
}

/**
 * 検証フラグを、原因と対処が読み取れる粒度の診断へ展開する。
 * ProjectEntry のフラグ単体では「開けるか」しか分からないため、意味付けはUI側で持つ。
 */
function diagnose(project: ProjectEntry, hubVersion: string): Diagnostic[] {
  if (project.validationPending) return [];
  if (!project.pathExists) {
    return [{ severity: 'error', label: 'フォルダーが見つかりません', detail: '登録されたパスが存在しません。移動されたか削除されています。' }];
  }
  const diagnostics: Diagnostic[] = [];
  if (!project.projectFileValid) diagnostics.push({ severity: 'error', label: '.fbzz_proj が不正', detail: 'name と project_id を読み取れませんでした。' });
  if (!project.layoutValid) diagnostics.push({ severity: 'error', label: '必須フォルダーが不足', detail: 'Assets / Src / Include のいずれかがありません。' });
  if (!project.cmakeExists) diagnostics.push({ severity: 'warning', label: 'CMakeLists.txt がありません', detail: 'スクリプト DLL をビルドできません。' });
  if (project.engineVersionMismatch) diagnostics.push({ severity: 'warning', label: `Engine ${project.engineVersion} 向け`, detail: `GameHub は ${hubVersion} です。マイグレーションが必要になる場合があります。` });
  else if (project.migrationRequired) diagnostics.push({ severity: 'warning', label: 'SDK が固定されていません', detail: '.fbzz_proj に sdk_id がないため、設定中の SDK で開きます。' });
  if (!project.generatedRootsExist) diagnostics.push({ severity: 'info', label: '未ビルド', detail: 'Lib / Binaries / Build は初回ビルドで生成されます。' });
  return diagnostics;
}

function statusOf(project: ProjectEntry, diagnostics: Diagnostic[]): ProjectStatus {
  if (project.validationPending) return 'pending';
  if (diagnostics.some((diagnostic) => diagnostic.severity === 'error')) return 'error';
  if (diagnostics.some((diagnostic) => diagnostic.severity === 'warning')) return 'warning';
  return 'ready';
}

function toneOf(status: ProjectStatus): string {
  return status === 'ready' ? 'ok' : status === 'warning' ? 'warn' : status === 'error' ? 'danger' : '';
}

function StatusChip({ status, issues }: { status: ProjectStatus; issues: number }) {
  return <span className={`chip ${status}`}>
    <i className={`dot ${toneOf(status)}`} />
    {STATUS_LABEL[status]}
    {issues > 0 && <em>{issues}</em>}
  </span>;
}

function Thumbnail({ project, className }: { project: ProjectEntry; className: string }) {
  const style = project.thumbnailDataUrl ? { backgroundImage: `url(${project.thumbnailDataUrl})` } : undefined;
  return <div className={className} style={style}>{!project.thumbnailDataUrl && project.name.slice(0, 2).toUpperCase()}</div>;
}

function ProjectRow({ entry, expanded, onToggle, onOpen, onReveal, onRemove }: {
  entry: InspectedProject;
  expanded: boolean;
  onToggle: () => void;
  onOpen: () => void;
  onReveal: () => void;
  onRemove: () => void;
}) {
  const { project, diagnostics, status } = entry;
  const opened = formatTimestamp(project.lastOpened);
  const issues = diagnostics.filter((diagnostic) => diagnostic.severity !== 'info').length;
  const canOpen = status === 'ready' || status === 'warning';

  return <>
    <div
      className={`row-grid project-row ${expanded ? 'expanded' : ''} ${status === 'error' ? 'blocked' : ''}`}
      role="button"
      tabIndex={0}
      aria-expanded={expanded}
      onClick={onToggle}
      onKeyDown={(event) => { if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); onToggle(); } }}
    >
      <Thumbnail project={project} className="row-thumb" />
      <div className="truncate">
        <div className="project-name truncate">{project.name}</div>
        <div className="row-sub truncate mono">{project.path}</div>
      </div>
      <StatusChip status={status} issues={issues} />
      <span className="row-cell col-engine num">{project.engineVersion}</span>
      <span className="row-cell col-sdk mono" title={project.sdkId}>{project.sdkId || '—'}</span>
      <span className="row-cell num" title={opened.title}>{opened.label}</span>
      <div className="row-actions" onClick={(event) => event.stopPropagation()}>
        <button className="button icon-only" onClick={onReveal} title="Explorer で開く"><Icon name="folder" size={14} /></button>
        <button className="button primary" onClick={onOpen} disabled={!canOpen} title={canOpen ? 'Editor で開く' : 'エラーを解消してください'}>
          <Icon name="play" size={12} />開く
        </button>
      </div>
    </div>

    {expanded && <div className="row-detail">
      <div className="row-detail-grid">
        <div className="detail-field"><span>パス</span><strong className="mono" title={project.path}>{project.path}</strong></div>
        <div className="detail-field"><span>Project ID</span><strong className="mono">{project.projectId || '—'}</strong></div>
        <div className="detail-field"><span>Engine / SDK</span><strong className="mono">{project.engineVersion} / {project.sdkId || '未固定'}</strong></div>
        <div className="detail-field"><span>最終起動</span><strong>{opened.title}</strong></div>
      </div>
      {diagnostics.length > 0 && <div className="diagnostics">
        {diagnostics.map((diagnostic) => <div className={`diagnostic ${diagnostic.severity}`} key={diagnostic.label}>
          <i className={`dot ${diagnostic.severity === 'error' ? 'danger' : diagnostic.severity === 'warning' ? 'warn' : ''}`} />
          <strong>{diagnostic.label}</strong>
          <span>{diagnostic.detail}</span>
        </div>)}
      </div>}
      <div className="card-foot">
        <button className="button" onClick={onReveal}><Icon name="folder" size={13} />Explorer で開く</button>
        <div className="spacer" />
        <button className="button danger" onClick={onRemove}><Icon name="trash" size={13} />一覧から削除</button>
      </div>
    </div>}
  </>;
}

function ProjectCard({ entry, onOpen, onReveal }: { entry: InspectedProject; onOpen: () => void; onReveal: () => void }) {
  const { project, diagnostics, status } = entry;
  const opened = formatTimestamp(project.lastOpened);
  const issues = diagnostics.filter((diagnostic) => diagnostic.severity !== 'info').length;
  const canOpen = status === 'ready' || status === 'warning';

  return <article className="project-card">
    <div className="card-art-wrap">
      <Thumbnail project={project} className="card-art" />
      <StatusChip status={status} issues={issues} />
    </div>
    <div className="card-body">
      <h3 className="truncate" title={project.name}>{project.name}</h3>
      <div className="card-meta">
        <span className="mono">{project.engineVersion}</span>
        <span>/</span>
        <span className="mono truncate">{project.sdkId || 'SDK 未固定'}</span>
      </div>
      <div className="row-sub truncate mono" title={project.path}>{project.path}</div>
      <div className="card-foot">
        <time title={opened.title}>{opened.label}</time>
        <div className="spacer" />
        <button className="button icon-only" onClick={onReveal} title="Explorer で開く"><Icon name="folder" size={14} /></button>
        <button className="button primary" onClick={onOpen} disabled={!canOpen}><Icon name="play" size={12} />開く</button>
      </div>
    </div>
  </article>;
}

function ProjectsPage({ entries, busy, onRefresh, onOpen, onReveal, onRemove }: {
  entries: InspectedProject[];
  busy: boolean;
  onRefresh: () => void;
  onOpen: (project: ProjectEntry) => void;
  onReveal: (project: ProjectEntry) => void;
  onRemove: (project: ProjectEntry) => void;
}) {
  const [query, setQuery] = useState('');
  const [filter, setFilter] = useState<StatusFilter>('all');
  const [sort, setSort] = useState<SortKey>('recent');
  const [view, setView] = useState<ViewMode>('list');
  const [expanded, setExpanded] = useState('');
  const searchRef = useRef<HTMLInputElement>(null);

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'f' && (event.ctrlKey || event.metaKey)) {
        event.preventDefault();
        searchRef.current?.focus();
      }
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, []);

  const counts = useMemo(() => ({
    all: entries.length,
    ready: entries.filter((entry) => entry.status === 'ready').length,
    issues: entries.filter((entry) => entry.status === 'warning' || entry.status === 'error').length,
  }), [entries]);

  const visible = useMemo(() => {
    const needle = query.trim().toLowerCase();
    return entries
      .filter((entry) => filter === 'all'
        || (filter === 'ready' && entry.status === 'ready')
        || (filter === 'issues' && (entry.status === 'warning' || entry.status === 'error')))
      .filter((entry) => !needle || `${entry.project.name} ${entry.project.path} ${entry.project.sdkId}`.toLowerCase().includes(needle))
      .sort((left, right) => {
        if (sort === 'name') return left.project.name.localeCompare(right.project.name, 'ja');
        if (sort === 'status' && left.status !== right.status) return STATUS_ORDER[left.status] - STATUS_ORDER[right.status];
        return right.project.lastOpened.localeCompare(left.project.lastOpened);
      });
  }, [entries, query, filter, sort]);

  return <>
    <div className="toolbar">
      <div className="search-box">
        <Icon name="search" size={14} />
        <input ref={searchRef} value={query} placeholder="名前・パス・SDK を検索" onChange={(event) => setQuery(event.target.value)} />
        <kbd>Ctrl+F</kbd>
      </div>
      <div className="segmented">
        {([['all', 'すべて'], ['ready', '正常'], ['issues', '要確認']] as const).map(([key, label]) =>
          <button key={key} className={filter === key ? 'active' : ''} onClick={() => setFilter(key)}>{label}<em>{counts[key]}</em></button>)}
      </div>
      <div className="spacer" />
      <select className="select" value={sort} onChange={(event) => setSort(event.target.value as SortKey)} title="並び順">
        <option value="recent">最終起動順</option>
        <option value="name">名前順</option>
        <option value="status">状態順</option>
      </select>
      <div className="segmented compact">
        <button className={view === 'list' ? 'active' : ''} onClick={() => setView('list')} title="リスト表示"><Icon name="list" size={14} /></button>
        <button className={view === 'grid' ? 'active' : ''} onClick={() => setView('grid')} title="グリッド表示"><Icon name="grid" size={14} /></button>
      </div>
      <button className="button icon-only" onClick={onRefresh} disabled={busy} title="再検証"><Icon name="refresh" size={14} /></button>
    </div>

    {visible.length === 0 && <div className="empty-state">
      <strong>{entries.length === 0 ? 'プロジェクトがありません' : '条件に一致するプロジェクトがありません'}</strong>
      <p>{entries.length === 0 ? 'テンプレートから作成するか、既存フォルダーを登録してください。' : '検索語やフィルターを変更してください。'}</p>
    </div>}

    {visible.length > 0 && view === 'list' && <div>
      <div className="row-grid table-head">
        <span />
        <button className={sort === 'name' ? 'sorted' : ''} onClick={() => setSort('name')}>名前 / パス</button>
        <button className={sort === 'status' ? 'sorted' : ''} onClick={() => setSort('status')}>状態</button>
        <span className="col-engine">Engine</span>
        <span className="col-sdk">SDK</span>
        <button className={sort === 'recent' ? 'sorted' : ''} onClick={() => setSort('recent')}>最終起動</button>
        <span className="right">操作</span>
      </div>
      {visible.map((entry) => <ProjectRow
        key={entry.project.path}
        entry={entry}
        expanded={expanded === entry.project.path}
        onToggle={() => setExpanded(expanded === entry.project.path ? '' : entry.project.path)}
        onOpen={() => onOpen(entry.project)}
        onReveal={() => onReveal(entry.project)}
        onRemove={() => onRemove(entry.project)}
      />)}
    </div>}

    {visible.length > 0 && view === 'grid' && <div className="project-grid">
      {visible.map((entry) => <ProjectCard key={entry.project.path} entry={entry} onOpen={() => onOpen(entry.project)} onReveal={() => onReveal(entry.project)} />)}
    </div>}
  </>;
}

function TemplatesPage({ templates, onUse }: { templates: TemplateInfo[]; onUse: (templateId: string) => void }) {
  if (templates.length === 0) {
    return <div className="empty-state">
      <strong>テンプレートが見つかりません</strong>
      <p>GameHub の Templates ディレクトリを確認してください。</p>
    </div>;
  }
  return <div className="template-list">
    {templates.map((template) => <button className="template-row" key={template.id} onClick={() => onUse(template.id)}>
      <span className="glyph">{template.displayName.slice(0, 1).toUpperCase()}</span>
      <div className="truncate">
        <h3 className="truncate">{template.displayName}</h3>
        <div className="row-sub mono truncate">{template.id}</div>
      </div>
      <p className="truncate">{template.description || '説明はありません。'}</p>
      <Icon name="arrow" size={15} />
    </button>)}
  </div>;
}

function SettingsPage({ settings, projectCount, hubVersion, onSave }: {
  settings: HubSettings;
  projectCount: number;
  hubVersion: string;
  onSave: (settings: HubSettings) => Promise<void>;
}) {
  const [draft, setDraft] = useState(settings);
  const [saving, setSaving] = useState(false);
  useEffect(() => setDraft(settings), [settings]);

  const dirty = JSON.stringify(draft) !== JSON.stringify(settings);
  const sdkPending = draft.sdkRoot.trim() !== settings.sdkRoot;
  const patch = (values: Partial<HubSettings>) => setDraft({ ...draft, ...values });

  return <div className="settings">
    <section className="card">
      <header><h2>ツールチェーン</h2><p>Editor の実体と、プロジェクトを開く SDK を決めます。</p></header>
      <div className="card-content">
        <div className="field">
          <div className="field-label"><span>Editor 実行ファイル</span><small>空欄なら SDK から自動検出</small></div>
          <div className="field-control">
            <div className="path-input">
              <input className="text-input mono" value={draft.editorExe} placeholder="(自動検出)" onChange={(event) => patch({ editorExe: event.target.value })} />
              <button className="button" onClick={async () => { const value = await window.gameHub.chooseEditorExecutable(); if (value) patch({ editorExe: value }); }}>参照</button>
            </div>
            <span className="hint">
              <Icon name="info" size={13} />
              {draft.editorExe ? '指定した実行ファイルを優先します。' : `tools/${draft.sdkConfiguration}/Editor/FBZZEditor.exe を使用します。`}
            </span>
          </div>
        </div>

        <div className="field">
          <div className="field-label"><span>FBZZ SDK</span><small>公開済み SDK のルート</small></div>
          <div className="field-control">
            <div className="path-input">
              <input className="text-input mono" value={draft.sdkRoot} placeholder={`SDK/${hubVersion}`} onChange={(event) => patch({ sdkRoot: event.target.value, sdkId: '' })} />
              <button className="button" onClick={async () => { const value = await window.gameHub.chooseDirectory(); if (value) patch({ sdkRoot: value, sdkId: '' }); }}>参照</button>
            </div>
            {sdkPending
              ? <span className="hint warn"><Icon name="alert" size={13} />保存時に fbzz-sdk.toml を検証します。</span>
              : settings.sdkId
                ? <span className="hint ok"><Icon name="check" size={13} />解決済み: {settings.sdkId}</span>
                : <span className="hint danger"><Icon name="alert" size={13} />SDK が未解決です。プロジェクトの作成と起動ができません。</span>}
          </div>
        </div>

        <div className="field">
          <div className="field-label"><span>Editor ビルド構成</span><small>起動する Editor のバイナリ</small></div>
          <div className="field-control">
            <div className="segmented self-start">
              {CONFIGURATIONS.map((configuration) => <button
                key={configuration}
                className={draft.sdkConfiguration === configuration ? 'active' : ''}
                onClick={() => patch({ sdkConfiguration: configuration })}
              >{configuration}</button>)}
            </div>
          </div>
        </div>
      </div>
    </section>

    <section className="card">
      <header><h2>外観</h2><p>System は OS の配色設定に追従します。</p></header>
      <div className="card-content">
        <div className="field">
          <div className="field-label"><span>テーマ</span></div>
          <div className="field-control">
            <div className="theme-row">
              {(Object.keys(THEME_LABEL) as HubTheme[]).map((theme) => <button
                key={theme}
                className={`theme-choice ${draft.theme === theme ? 'selected' : ''}`}
                onClick={() => patch({ theme })}
              >
                <i className={`theme-preview ${theme}`}><i /><i /></i>
                <span>{THEME_LABEL[theme]}</span>
              </button>)}
            </div>
          </div>
        </div>
      </div>
    </section>

    <section className="card">
      <header><h2>環境</h2><p>保存済みの値です。</p></header>
      <div className="card-content">
        <dl className="readout">
          <dt>GameHub</dt><dd className="mono">v{hubVersion}</dd>
          <dt>解決済み SDK ID</dt><dd className="mono">{settings.sdkId || '—'}</dd>
          <dt>SDK ルート</dt><dd className="mono" title={settings.sdkRoot}>{settings.sdkRoot || '—'}</dd>
          <dt>Editor 実行ファイル</dt><dd className="mono" title={settings.editorExe}>{settings.editorExe || '(自動検出)'}</dd>
          <dt>登録プロジェクト</dt><dd>{projectCount}</dd>
        </dl>
      </div>
    </section>

    <div className="settings-footer">
      <span className="hint">
        {dirty
          ? <><Icon name="alert" size={13} />未保存の変更があります。</>
          : <><Icon name="check" size={13} />保存済み</>}
      </span>
      <button className="button" disabled={!dirty || saving} onClick={() => setDraft(settings)}>変更を破棄</button>
      <button className="button primary" disabled={!dirty || saving} onClick={async () => { setSaving(true); try { await onSave(draft); } finally { setSaving(false); } }}>
        {saving ? '保存中…' : '設定を保存'}
      </button>
    </div>
  </div>;
}

function CreateDialog({ templates, settings, initialTemplateId, onClose, onCreate }: {
  templates: TemplateInfo[];
  settings: HubSettings;
  initialTemplateId: string;
  onClose: () => void;
  onCreate: (request: CreateProjectRequest) => Promise<void>;
}) {
  const [displayName, setDisplayName] = useState('My Game');
  const [destinationRoot, setDestinationRoot] = useState('');
  const [templateId, setTemplateId] = useState(initialTemplateId || templates[0]?.id || 'standard');
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => { if (event.key === 'Escape') onClose(); };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [onClose]);

  // main 側と同じ導出規則で、作成前にフォルダー名と識別子を提示する。
  const identifiers = deriveProjectIdentifiers(displayName);
  const nameValid = isValidProjectIdentifiers(identifiers);
  const canSubmit = !busy && nameValid && Boolean(destinationRoot) && Boolean(settings.sdkId);

  const submit = async () => {
    setBusy(true);
    try { await onCreate({ displayName, destinationRoot, templateId }); } finally { setBusy(false); }
  };

  return <div className="modal-backdrop" onMouseDown={onClose}>
    <section className="modal" onMouseDown={(event) => event.stopPropagation()}>
      <header>
        <h2>新規プロジェクト</h2>
        <button className="button quiet icon-only" onClick={onClose} title="閉じる"><Icon name="close" size={15} /></button>
      </header>

      <div className="modal-body">
        <div className="field">
          <div className="field-label"><span>プロジェクト名</span></div>
          <div className="field-control">
            <input className="text-input" value={displayName} autoFocus onChange={(event) => setDisplayName(event.target.value)} />
            {nameValid
              ? <span className="hint">ターゲット <b className="mono">{identifiers.targetName}</b> / 識別子 <b className="mono">{identifiers.projectId}</b></span>
              : <span className="hint danger"><Icon name="alert" size={13} />英字で始まる ASCII 英数字を含めてください。</span>}
          </div>
        </div>

        <div className="field">
          <div className="field-label"><span>作成先</span><small>親フォルダー</small></div>
          <div className="field-control">
            <div className="path-input">
              <input className="text-input mono" value={destinationRoot} placeholder="親フォルダーを選択" onChange={(event) => setDestinationRoot(event.target.value)} />
              <button className="button" onClick={async () => { const value = await window.gameHub.chooseDirectory(); if (value) setDestinationRoot(value); }}>参照</button>
            </div>
            {destinationRoot && nameValid && <div className="preview-path mono">{destinationRoot}\<b>{identifiers.targetName}</b></div>}
          </div>
        </div>

        <div className="field">
          <div className="field-label"><span>テンプレート</span></div>
          <div className="field-control">
            <div className="template-picker">
              {templates.map((template) => <button
                key={template.id}
                className={`template-option ${templateId === template.id ? 'selected' : ''}`}
                onClick={() => setTemplateId(template.id)}
              >
                <strong>{template.displayName}</strong>
                <small className="truncate">{template.description}</small>
              </button>)}
            </div>
          </div>
        </div>

        <div className="field">
          <div className="field-label"><span>使用 SDK</span></div>
          <div className="field-control">
            <span className={`hint ${settings.sdkId ? 'ok' : 'danger'}`}>
              <Icon name={settings.sdkId ? 'check' : 'alert'} size={13} />
              {settings.sdkId || '未解決'} / {settings.sdkConfiguration}
            </span>
          </div>
        </div>
      </div>

      <footer>
        {!settings.sdkId && <span className="hint danger"><Icon name="alert" size={13} />設定で FBZZ SDK を指定してください。</span>}
        <button className="button" onClick={onClose}>キャンセル</button>
        <button className="button primary" disabled={!canSubmit} onClick={submit}>{busy ? '作成中…' : '作成'}</button>
      </footer>
    </section>
  </div>;
}

export function App() {
  const [data, setData] = useState<BootstrapData | null>(null);
  const [page, setPage] = useState<Page>('projects');
  const [creating, setCreating] = useState('');
  const [busy, setBusy] = useState(false);
  const [notice, setNotice] = useState('');
  const [error, setError] = useState('');
  const refreshGeneration = useRef(0);
  const initialLoadStarted = useRef(false);
  const resolvedSettings = useRef<HubSettings | null>(null);

  const refresh = useCallback(async () => {
    const generation = ++refreshGeneration.current;
    setBusy(true);
    const result = await window.gameHub.bootstrap();
    if (generation !== refreshGeneration.current) return;
    if (!result.ok || !result.value) {
      setBusy(false);
      setError(result.error ?? 'GameHub を初期化できませんでした。');
      return;
    }

    // 設定・テンプレート・仮カードを先に表示し、重い検証結果は後から差し替える。
    setData({ ...result.value, settings: resolvedSettings.current ?? result.value.settings });
    void window.gameHub.listProjects().then((projectsResult) => {
      if (generation !== refreshGeneration.current) return;
      setBusy(false);
      if (projectsResult.ok && projectsResult.value) {
        setData((current) => current ? { ...current, projects: projectsResult.value ?? [] } : current);
      } else {
        setError(projectsResult.error ?? 'プロジェクト情報を更新できませんでした。');
      }
    });
  }, []);

  useEffect(() => {
    const unsubscribe = window.gameHub.onSettingsUpdated((settings) => {
      resolvedSettings.current = settings;
      setData((current) => current ? { ...current, settings } : current);
    });
    return unsubscribe;
  }, []);

  useEffect(() => {
    // React.StrictMode は開発時に effect を二度実行する。
    // WHY: refresh はIPCとプロジェクト検証を開始する副作用なので、再実行すると
    //      起動直後に同じディスク走査を二重に発生させてしまう。
    if (initialLoadStarted.current) return;
    initialLoadStarted.current = true;
    void refresh();
  }, [refresh]);

  // system は CSS に持たせず、ここで実際の配色へ解決する。
  useEffect(() => {
    const choice = data?.settings.theme ?? 'modern';
    const media = window.matchMedia('(prefers-color-scheme: light)');
    const apply = () => { document.documentElement.dataset.theme = choice === 'system' ? (media.matches ? 'light' : 'modern') : choice; };
    apply();
    media.addEventListener('change', apply);
    return () => media.removeEventListener('change', apply);
  }, [data?.settings.theme]);

  useEffect(() => {
    if (!notice && !error) return;
    const timer = window.setTimeout(() => { setNotice(''); setError(''); }, 6_000);
    return () => window.clearTimeout(timer);
  }, [notice, error]);

  const entries = useMemo<InspectedProject[]>(() => {
    if (!data) return [];
    return data.projects.map((project) => {
      const diagnostics = diagnose(project, data.engineVersion);
      return { project, diagnostics, status: statusOf(project, diagnostics) };
    });
  }, [data]);

  const run = async (operation: () => Promise<{ ok: boolean; error?: string }>, message?: string) => {
    setError('');
    const result = await operation();
    if (!result.ok) setError(result.error ?? '操作に失敗しました。');
    else { if (message) setNotice(message); await refresh(); }
    return result.ok;
  };

  if (!data) {
    return <div className="loading-screen">
      <Logo size={40} />
      <p>ワークスペースを読み込んでいます…</p>
      {error && <span className="error-text">{error}</span>}
    </div>;
  }

  const { settings } = data;
  const readyCount = entries.filter((entry) => entry.status === 'ready').length;
  const issueCount = entries.filter((entry) => entry.status === 'warning' || entry.status === 'error').length;
  const sdkTone = settings.sdkId ? 'ok' : 'danger';

  return <div className="app-shell">
    <header className="titlebar">
      <div className="drag-region">
        <Logo size={15} />
        <span>FBZZ GameHub</span>
        <b className="mono">v{data.engineVersion}</b>
      </div>
      <button className="titlebar-status" onClick={() => setPage('settings')} title="SDK 設定を開く">
        <i className={`dot ${sdkTone}`} />
        <span className="mono">{settings.sdkId || 'SDK 未解決'}</span>
      </button>
      <div className="window-controls">
        <button onClick={window.gameHub.minimizeWindow} title="最小化">−</button>
        <button onClick={window.gameHub.maximizeWindow} title="最大化">□</button>
        <button className="window-close" onClick={window.gameHub.closeWindow} title="閉じる">×</button>
      </div>
    </header>

    <aside className="sidebar">
      <div className="brand">
        <Logo size={24} />
        <div className="brand-text"><strong>FBZZ</strong><span>Engine Hub</span></div>
      </div>
      <nav>
        <button className={page === 'projects' ? 'active' : ''} onClick={() => setPage('projects')}>
          <Icon name="grid" /><span>プロジェクト</span><em>{entries.length}</em>
        </button>
        <button className={page === 'templates' ? 'active' : ''} onClick={() => setPage('templates')}>
          <Icon name="layers" /><span>テンプレート</span><em>{data.templates.length}</em>
        </button>
        <button className={page === 'settings' ? 'active' : ''} onClick={() => setPage('settings')}>
          <Icon name="settings" /><span>設定</span>
        </button>
      </nav>

      <button className="sdk-panel" onClick={() => setPage('settings')} title="設定を開く">
        <div className="sdk-panel-head"><Icon name="cpu" size={14} />Engine SDK<i className={`dot ${sdkTone}`} /></div>
        <dl>
          <dt>SDK</dt><dd className="strong mono">{settings.sdkId || '未解決'}</dd>
          <dt>構成</dt><dd>{settings.sdkConfiguration}</dd>
          <dt>Editor</dt><dd>{settings.editorExe ? '手動指定' : '自動検出'}</dd>
          <dt>Root</dt><dd title={settings.sdkRoot}>{settings.sdkRoot || '未設定'}</dd>
        </dl>
      </button>
    </aside>

    <main className="content">
      <div className="page-head">
        <div>
          <h1>{page === 'projects' ? 'プロジェクト' : page === 'templates' ? 'テンプレート' : '設定'}</h1>
          {page === 'projects' && <div className="page-summary">
            <b>{entries.length}</b> プロジェクト<span className="sep">·</span>
            正常 <b>{readyCount}</b><span className="sep">·</span>
            要確認 <b>{issueCount}</b><span className="sep">·</span>
            SDK <b className="mono">{settings.sdkId || '未解決'}</b> / {settings.sdkConfiguration}
          </div>}
          {page === 'templates' && <div className="page-summary">
            <b>{data.templates.length}</b> テンプレート<span className="sep">·</span>新規プロジェクトの雛形
          </div>}
          {page === 'settings' && <div className="page-summary">hub_config.toml に保存されます</div>}
        </div>
        {page !== 'settings' && <div className="page-head-actions">
          <button className="button" onClick={() => void run(() => window.gameHub.addProject(), '既存プロジェクトを追加しました')}>
            <Icon name="plus" size={13} />既存を追加
          </button>
          <button className="button primary" onClick={() => setCreating(data.templates[0]?.id ?? 'standard')}>
            <Icon name="plus" size={13} />新規プロジェクト
          </button>
        </div>}
      </div>

      {page === 'projects' && <ProjectsPage
        entries={entries}
        busy={busy}
        onRefresh={() => void refresh()}
        onOpen={(project) => void run(() => window.gameHub.openProject(project.path), `${project.name} を起動しました`)}
        onReveal={(project) => void run(() => window.gameHub.revealProject(project.path))}
        onRemove={(project) => void run(() => window.gameHub.removeProject(project.path), `${project.name} を一覧から削除しました`)}
      />}
      {page === 'templates' && <TemplatesPage templates={data.templates} onUse={(templateId) => setCreating(templateId)} />}
      {page === 'settings' && <SettingsPage
        settings={settings}
        projectCount={data.projects.length}
        hubVersion={data.engineVersion}
        onSave={async (next) => { await run(() => window.gameHub.saveSettings(next), '設定を保存しました'); }}
      />}
    </main>

    {(error || notice) && <button className={`toast ${error ? 'error' : ''}`} onClick={() => { setError(''); setNotice(''); }}>
      <Icon name={error ? 'alert' : 'check'} size={14} />
      <span>{error || notice}</span>
    </button>}

    {creating && <CreateDialog
      templates={data.templates}
      settings={settings}
      initialTemplateId={creating}
      onClose={() => setCreating('')}
      onCreate={async (request) => {
        const ok = await run(() => window.gameHub.createProject(request), `${request.displayName} を作成しました`);
        if (ok) { setCreating(''); setPage('projects'); }
      }}
    />}
  </div>;
}
