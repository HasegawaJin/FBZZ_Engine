// FBZZ GameHub
// App.tsx | renderer
// プロジェクト管理、作成、設定画面をまとめるGameHubのルートUI

import { useEffect, useMemo, useRef, useState } from 'react';
import type { BootstrapData, CreateProjectRequest, HubSettings, ProjectEntry, TemplateInfo } from '../shared/contracts';
import { Icon } from './Icon';
import { Logo } from './Logo';

type Page = 'projects' | 'templates' | 'settings';

function formatLastOpened(value: string): string {
  if (!value) return 'Never opened';
  const date = new Date(value);
  return Number.isNaN(date.getTime()) ? value : new Intl.DateTimeFormat('ja-JP', { dateStyle: 'medium', timeStyle: 'short' }).format(date);
}

function ProjectCard({ project, onOpen, onReveal, onRemove }: {
  project: ProjectEntry;
  onOpen: () => void;
  onReveal: () => void;
  onRemove: () => void;
}) {
  const healthy = !project.validationPending && project.pathExists && project.projectFileValid && project.layoutValid;
  const statusClass = project.validationPending ? 'pending' : healthy ? 'healthy' : 'warning';
  const statusText = project.validationPending ? 'Inspecting…' : healthy ? 'Ready' : 'Check project';
  return (
    <article className="project-card">
      <div className="project-art" style={project.thumbnailDataUrl ? { backgroundImage: `url(${project.thumbnailDataUrl})` } : undefined}>
        {!project.thumbnailDataUrl && <div className="project-monogram">{project.name.slice(0, 2).toUpperCase()}</div>}
        <span className={`status-pill ${statusClass}`}>{statusText}</span>
      </div>
      <div className="project-body">
        <div className="project-heading">
          <div><h3>{project.name}</h3><p>FBZZ {project.engineVersion}{project.sdkId ? ` · ${project.sdkId}` : ''}</p></div>
          <button className="icon-button" onClick={onReveal} title="Explorerで表示"><Icon name="folder" size={15} /></button>
        </div>
        <p className="project-path" title={project.path}>{project.path}</p>
        <div className="project-footer">
          <span>{formatLastOpened(project.lastOpened)}</span>
          <div className="card-actions">
            <button className="remove-button" onClick={onRemove} title="一覧から削除">×</button>
            <button className="open-button" onClick={onOpen} disabled={!healthy}><Icon name="play" size={13} /><span>Open</span></button>
          </div>
        </div>
      </div>
    </article>
  );
}

function WorkspaceHero({ data, onCreate, onTemplates }: { data: BootstrapData; onCreate: () => void; onTemplates: () => void }) {
  const readyCount = data.projects.filter((project) => project.pathExists && project.projectFileValid && project.layoutValid).length;
  return <section className="workspace-hero">
    <div className="hero-orbit orbit-one" />
    <div className="hero-orbit orbit-two" />
    <div className="hero-copy">
      <span className="hero-kicker"><Icon name="sparkles" size={13} /> FBZZ CREATOR WORKSPACE</span>
      <h2>Turn an idea into<br /><em>a playable world.</em></h2>
      <p>テンプレートを選び、プロジェクトを作成して、そのままEditorへ。</p>
      <div className="hero-actions"><button className="hero-primary" onClick={onCreate}>Create project <Icon name="arrow" size={14} /></button><button className="hero-link" onClick={onTemplates}>Browse templates</button></div>
    </div>
    <div className="hero-metrics">
      <div><span>PROJECTS</span><strong>{data.projects.length.toString().padStart(2, '0')}</strong><small>in workspace</small></div>
      <div><span>READY</span><strong>{readyCount.toString().padStart(2, '0')}</strong><small>validated</small></div>
      <div className={data.settings.sdkRoot ? 'metric-online' : 'metric-offline'}><span>SDK</span><strong><i />{data.settings.sdkRoot ? 'LIVE' : 'OFF'}</strong><small>{data.settings.sdkId || `v${data.engineVersion}`}</small></div>
    </div>
  </section>;
}

function CreateDialog({ templates, onClose, onCreate }: {
  templates: TemplateInfo[];
  onClose: () => void;
  onCreate: (request: CreateProjectRequest) => Promise<void>;
}) {
  const [displayName, setDisplayName] = useState('My Game');
  const [destinationRoot, setDestinationRoot] = useState('');
  const [templateId, setTemplateId] = useState(templates[0]?.id ?? 'standard');
  const [busy, setBusy] = useState(false);

  const chooseDestination = async () => {
    const selected = await window.gameHub.chooseDirectory();
    if (selected) setDestinationRoot(selected);
  };
  const submit = async () => {
    setBusy(true);
    try { await onCreate({ displayName, destinationRoot, templateId }); } finally { setBusy(false); }
  };

  return (
    <div className="modal-backdrop" onMouseDown={onClose}>
      <section className="modal create-modal" onMouseDown={(event) => event.stopPropagation()}>
        <div className="modal-header"><div><span className="eyebrow">NEW WORKSPACE</span><h2>Create a project</h2></div><button className="icon-button" onClick={onClose}>×</button></div>
        <label className="field"><span>Project name</span><input value={displayName} onChange={(event) => setDisplayName(event.target.value)} autoFocus /></label>
        <label className="field"><span>Location</span><div className="path-input"><input value={destinationRoot} onChange={(event) => setDestinationRoot(event.target.value)} placeholder="Choose a parent folder" /><button onClick={chooseDestination}>Browse</button></div></label>
        <div className="field"><span>Template</span><div className="template-options">
          {templates.map((template) => <button key={template.id} className={`template-option ${templateId === template.id ? 'selected' : ''}`} onClick={() => setTemplateId(template.id)}><strong>{template.displayName}</strong><small>{template.description}</small></button>)}
        </div></div>
        <div className="modal-actions"><button className="secondary-button" onClick={onClose}>Cancel</button><button className="primary-button" disabled={busy || !displayName.trim() || !destinationRoot} onClick={submit}>{busy ? 'Creating…' : 'Create project'}</button></div>
      </section>
    </div>
  );
}

function SettingsPage({ settings, onSave }: { settings: HubSettings; onSave: (settings: HubSettings) => Promise<void> }) {
  const [draft, setDraft] = useState(settings);
  useEffect(() => setDraft(settings), [settings]);
  return <div className="settings-panel">
    <div className="section-title"><span className="eyebrow">CONFIGURATION</span><h1>Settings</h1><p>Editorと共有SDKの場所を設定します。</p></div>
    <section className="settings-card"><h3>Toolchain</h3>
      <label className="field"><span>Editor executable</span><div className="path-input"><input value={draft.editorExe} onChange={(event) => setDraft({ ...draft, editorExe: event.target.value })} placeholder="Automatic discovery" /><button onClick={async () => { const value = await window.gameHub.chooseEditorExecutable(); if (value) setDraft({ ...draft, editorExe: value }); }}>Browse</button></div></label>
      <label className="field"><span>Published FBZZ SDK</span><div className="path-input"><input value={draft.sdkRoot} onChange={(event) => setDraft({ ...draft, sdkRoot: event.target.value, sdkId: '' })} placeholder="SDK/0.1.0" /><button onClick={async () => { const value = await window.gameHub.chooseDirectory(); if (value) setDraft({ ...draft, sdkRoot: value, sdkId: '' }); }}>Browse</button></div></label>
      <label className="field"><span>Editor configuration</span><select value={draft.sdkConfiguration} onChange={(event) => setDraft({ ...draft, sdkConfiguration: event.target.value as HubSettings['sdkConfiguration'] })}><option value="Debug">Debug</option><option value="Development">Development</option><option value="Release">Release</option></select></label>
    </section>
    <section className="settings-card"><h3>Appearance</h3><div className="theme-row">
      {(['midnight', 'dark'] as const).map((theme) => <button key={theme} className={`theme-choice ${draft.theme === theme ? 'selected' : ''}`} onClick={() => setDraft({ ...draft, theme })}><i className={`theme-preview ${theme}`} /><span>{theme === 'midnight' ? 'Midnight violet' : 'Carbon dark'}</span></button>)}
    </div></section>
    <button className="primary-button save-settings" onClick={() => onSave(draft)}>Save settings</button>
  </div>;
}

export function App() {
  const [data, setData] = useState<BootstrapData | null>(null);
  const [page, setPage] = useState<Page>('projects');
  const [query, setQuery] = useState('');
  const [creating, setCreating] = useState(false);
  const [notice, setNotice] = useState('');
  const [error, setError] = useState('');
  const refreshGeneration = useRef(0);

  const refresh = async () => {
    const generation = ++refreshGeneration.current;
    const result = await window.gameHub.bootstrap();
    if (generation !== refreshGeneration.current) return;
    if (!result.ok || !result.value) {
      setError(result.error ?? 'GameHubを初期化できませんでした。');
      return;
    }

    // 設定・テンプレート・仮カードを先に表示し、重い検証結果は後から差し替える。
    setData(result.value);
    void window.gameHub.listProjects().then((projectsResult) => {
      if (generation !== refreshGeneration.current) return;
      if (projectsResult.ok && projectsResult.value) {
        setData((current) => current ? { ...current, projects: projectsResult.value ?? [] } : current);
      } else {
        setError(projectsResult.error ?? 'プロジェクト情報を更新できませんでした。');
      }
    });
  };
  useEffect(() => { void refresh(); }, []);
  useEffect(() => { if (data) document.documentElement.dataset.theme = data.settings.theme; }, [data?.settings.theme]);

  const projects = useMemo(() => data?.projects.filter((project) => `${project.name} ${project.path}`.toLowerCase().includes(query.toLowerCase())) ?? [], [data?.projects, query]);
  const run = async (operation: () => Promise<{ ok: boolean; error?: string }>, message?: string) => {
    setError('');
    const result = await operation();
    if (!result.ok) setError(result.error ?? '操作に失敗しました。');
    else { if (message) setNotice(message); await refresh(); }
    return result.ok;
  };

  if (!data) return <div className="loading-screen"><Logo /><p>Loading workspace…</p>{error && <span>{error}</span>}</div>;

  return <div className="app-shell">
    <div className="ambient-glow glow-violet" /><div className="ambient-glow glow-blue" /><div className="noise-layer" />
    <header className="titlebar"><div className="drag-region"><Logo compact /><span>FBZZ GameHub</span><b>v{data.engineVersion}</b></div><div className="window-controls"><button onClick={window.gameHub.minimizeWindow}>−</button><button onClick={window.gameHub.maximizeWindow}>□</button><button className="window-close" onClick={window.gameHub.closeWindow}>×</button></div></header>
    <aside className="sidebar">
      <div className="brand"><Logo /><div><strong>FBZZ</strong><span>ENGINE HUB</span></div></div>
      <nav>
        <button className={page === 'projects' ? 'active' : ''} onClick={() => setPage('projects')}><i><Icon name="grid" /></i>Projects<span>{data.projects.length}</span></button>
        <button className={page === 'templates' ? 'active' : ''} onClick={() => setPage('templates')}><i><Icon name="layers" /></i>Templates</button>
        <button className={page === 'settings' ? 'active' : ''} onClick={() => setPage('settings')}><i><Icon name="settings" /></i>Settings</button>
      </nav>
      <div className="engine-card"><div className="engine-card-icon"><Icon name="cpu" size={17} /></div><span>ENGINE SDK</span><strong><i className="live-dot" />{data.settings.sdkId || `FBZZ ${data.engineVersion}`}</strong><small>{data.settings.sdkRoot || 'SDK path not configured'}</small></div>
    </aside>
    <main className="content">
      {page === 'settings' ? <SettingsPage settings={data.settings} onSave={async (settings) => { await run(() => window.gameHub.saveSettings(settings), 'Settings saved'); }} /> : <>
        <div className="content-header"><div><span className="eyebrow">{page === 'projects' ? 'YOUR WORKSPACES' : 'STARTING POINTS'}</span><h1>{page === 'projects' ? 'Projects' : 'Templates'}</h1><p>{page === 'projects' ? 'Build something worth remembering.' : 'Choose a foundation for your next game.'}</p></div><div className="header-actions">{page === 'projects' && <div className="search-box"><Icon name="search" size={15} /><input placeholder="Search projects" value={query} onChange={(event) => setQuery(event.target.value)} /></div>}<button className="secondary-button" onClick={() => void run(() => window.gameHub.addProject(), 'Project added')}>Add existing</button><button className="primary-button" onClick={() => setCreating(true)}><Icon name="plus" size={14} /> New project</button></div></div>
        {page === 'projects' && <WorkspaceHero data={data} onCreate={() => setCreating(true)} onTemplates={() => setPage('templates')} />}
        {page === 'projects' ? <><div className="collection-heading"><div><span>RECENT</span><h2>Your projects</h2></div><small>{projects.length} workspace{projects.length === 1 ? '' : 's'}</small></div><div className="project-grid">{projects.map((project) => <ProjectCard key={project.path} project={project} onOpen={() => void run(() => window.gameHub.openProject(project.path), 'Editor launched')} onReveal={() => void run(() => window.gameHub.revealProject(project.path))} onRemove={() => void run(() => window.gameHub.removeProject(project.path), 'Project removed from the list')} />)}{projects.length === 0 && <button className="empty-state" onClick={() => setCreating(true)}><span><Icon name="plus" size={20} /></span><strong>Create your first project</strong><small>Start from an FBZZ template</small></button>}</div></> : <div className="template-gallery">{data.templates.map((template, index) => <button key={template.id} className="template-showcase" onClick={() => setCreating(true)}><div className={`template-art art-${index % 3}`}><span>{template.displayName.slice(0, 1)}</span><i><Icon name="sparkles" size={16} /></i></div><h3>{template.displayName}</h3><p>{template.description}</p><small>Use template <Icon name="arrow" size={13} /></small></button>)}</div>}
      </>}
    </main>
    {(error || notice) && <div className={`toast ${error ? 'error' : ''}`} onClick={() => { setError(''); setNotice(''); }}>{error || notice}</div>}
    {creating && <CreateDialog templates={data.templates} onClose={() => setCreating(false)} onCreate={async (request) => { const ok = await run(() => window.gameHub.createProject(request), 'Project created'); if (ok) { setCreating(false); setPage('projects'); } }} />}
  </div>;
}
