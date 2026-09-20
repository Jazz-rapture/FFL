(() => {
  const loginIcon = '../resoures/icon/steve.png';
  const tauri = window.__tauricpp__;

  // ---- 窗口操作 ----
  document.querySelector('.btn-blue').addEventListener('click', () => tauri.invoke('window.minimize'));
  document.querySelector('.btn-red').addEventListener('click', () => tauri.invoke('window.close'));

  const dragHandle = document.querySelector('.top-bar-drag');
  if (dragHandle) {
    dragHandle.addEventListener('mousedown', (e) => {
      if (e.button === 0) tauri.invoke('window.startDrag');
    });
  }

  // ---- DOM 引用 ----
  const layerLogin = document.getElementById('layerLogin');
  const layerTransition = document.getElementById('layerTransition');
  const layerLobby = document.getElementById('layerLobby');
  const loginCard = document.getElementById('loginCard');
  const loginHint = document.getElementById('loginHint');
  const loginInput = document.getElementById('loginInput');
  const loginBtn = document.getElementById('loginBtn');
  const loginAvatar = document.getElementById('loginAvatar');
  const transitionImg = document.getElementById('transitionImg');

  // ---- 图层切换（大厅层走单独的显示逻辑） ----
  function showLayer(layer) {
    [layerLogin, layerTransition].forEach(l => l.classList.remove('active'));
    layerLobby.classList.remove('active');
    if (layer) layer.classList.add('active');

    // 主界面元素动画控制
    const versionCard = document.querySelector('.version-card');
    const startGameBtn = document.querySelector('.start-game-btn');
    const accountCard = document.querySelector('.account-card');

    if (layer === layerLobby) {
      // 进入大厅：从下方滑入（每次都要重新滑入）
      // 先重置状态
      if (versionCard) {
        versionCard.classList.remove('slide-in', 'slide-out');
        void versionCard.offsetWidth; // 强制重排
        versionCard.classList.add('slide-in');
      }
      if (startGameBtn) {
        startGameBtn.classList.remove('slide-in', 'slide-out');
        void startGameBtn.offsetWidth;
        startGameBtn.classList.add('slide-in');
      }
      if (accountCard) {
        accountCard.classList.remove('slide-in', 'slide-out');
        void accountCard.offsetWidth;
        accountCard.classList.add('slide-in');
      }
    } else {
      // 离开大厅：向下滑出
      if (versionCard) {
        versionCard.classList.remove('slide-in');
        versionCard.classList.add('slide-out');
      }
      if (startGameBtn) {
        startGameBtn.classList.remove('slide-in');
        startGameBtn.classList.add('slide-out');
      }
      if (accountCard) {
        accountCard.classList.remove('slide-in');
        accountCard.classList.add('slide-out');
      }
    }
  }

  // ---- 启动入口 ----
  tauri.invoke('account.read').then((config) => {
    const hasAny = config && config.accounts && config.accounts.some(id => id && id.length > 0);

    if (hasAny) {
      startTransition();
    } else {
      showLayer(layerLogin);
    }
  }).catch(() => {
    showLayer(layerLogin);
  });

  // ---- 登录逻辑 ----
  loginBtn.addEventListener('click', doLogin);
  loginInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') doLogin();
  });

  function doLogin() {
    const id = loginInput.value.trim();
    if (!id) return;

    loginBtn.disabled = true;
    loginBtn.textContent = '登录中...';

    // 先读取现有账户，找到空槽位
    tauri.invoke('account.read').then((config) => {
      let emptyIndex = 0;
      if (config && config.accounts) {
        // 找到第一个空槽位
        for (let i = 0; i < config.accounts.length; i++) {
          if (!config.accounts[i] || config.accounts[i].length === 0) {
            emptyIndex = i;
            break;
          }
          emptyIndex = i + 1; // 如果都满了，就用下一个索引
        }
      }
      // 写入到空槽位
      return tauri.invoke('account.write', { index: emptyIndex, id: id, name: '' }).then((result) => {
        return { result, index: emptyIndex };
      });
    }).then(({ result, index }) => {
      if (result && result.success) {
        return tauri.invoke('account.check', { index: index });
      }
      return null;
    }).then((checkResult) => {
      if (checkResult && checkResult.has_account) {
        loginAvatar.src = loginIcon;
        startTransition();
      } else {
        loginBtn.textContent = '登录';
        loginBtn.disabled = false;
      }
    }).catch(() => {
      loginBtn.textContent = '登录';
      loginBtn.disabled = false;
    });
  }

  // ---- 过渡动画 ----
  function startTransition() {
    layerLogin.classList.remove('active');

    setTimeout(() => {
      layerTransition.classList.add('active');
    }, 400);

    setTimeout(() => {
      layerTransition.classList.add('fade-out');
    }, 1600);

    setTimeout(() => {
      layerTransition.classList.remove('active');
      layerTransition.classList.remove('fade-out');
      showLayer(layerLobby);
      // 重新加载账户卡片
      loadAccountCard();
    }, 2200);
  }

  // ---- 页面切换逻辑 ----
  const homePage = document.getElementById('homePage');
  const downloadPage = document.getElementById('downloadPage');
  const settingsPage = document.getElementById('settingsPage');
  const resourcePage = document.getElementById('resourcePage');
  const sidebarIcons = document.querySelectorAll('.sidebar-icon');

  // 初始化：主页激活，其它页面隐藏
  homePage.classList.add('active');
  downloadPage.style.opacity = '0';
  downloadPage.style.pointerEvents = 'none';
  settingsPage.style.opacity = '0';
  settingsPage.style.pointerEvents = 'none';
  resourcePage.style.opacity = '0';
  resourcePage.style.pointerEvents = 'none';

  function switchPage(pageName) {
    if (pageName === 'home' && homePage.classList.contains('active')) return;
    if (pageName === 'download' && downloadPage.classList.contains('active')) return;
    if (pageName === 'settings' && settingsPage.classList.contains('active')) return;
    if (pageName === 'resource' && resourcePage.classList.contains('active')) return;

    // 离开主页时，让版本卡片和角色卡片滑出
    if (homePage.classList.contains('active')) {
      homePage.style.opacity = '0';
      slideOutHomeCards();
    }
    
    // 离开有副导航栏的页面时，让副导航栏向左滑出
    slideOutSubSidebars();

    // 离开其他页面时淡出展示区
    if (downloadPage.classList.contains('active')) downloadPage.style.opacity = '0';
    if (settingsPage.classList.contains('active')) settingsPage.style.opacity = '0';
    if (resourcePage.classList.contains('active')) resourcePage.style.opacity = '0';

    // 立即开始加载新页面内容（不等待淡出完成）
    if (pageName === 'download') {
      activeCategory = 'game';
      subSidebarItems.forEach(i => {
        i.classList.toggle('active', i.dataset.category === 'game');
      });
      loadDownloadContent(activeCategory);
    } else if (pageName === 'settings') {
      loadSettingsContent(activeSettingsCategory);
    } else if (pageName === 'resource') {
      loadResourceContent(activeResourceCategory);
    }

    // 淡出完成后切换页面
    setTimeout(() => {
      homePage.classList.remove('active');
      downloadPage.classList.remove('active');
      settingsPage.classList.remove('active');
      resourcePage.classList.remove('active');
      homePage.style.pointerEvents = 'none';
      downloadPage.style.pointerEvents = 'none';
      settingsPage.style.pointerEvents = 'none';
      resourcePage.style.pointerEvents = 'none';

      if (pageName === 'home') {
        homePage.classList.add('active');
        homePage.style.opacity = '1';
        homePage.style.pointerEvents = '';
        // 切回主页时，让版本卡片和角色卡片重新滑入
        slideInHomeCards();
      } else if (pageName === 'download') {
        downloadPage.classList.add('active');
        downloadPage.style.opacity = '1';
        downloadPage.style.pointerEvents = '';
        // 副导航栏滑入
        slideInSubSidebar(downloadPage);
      } else if (pageName === 'settings') {
        settingsPage.classList.add('active');
        settingsPage.style.opacity = '1';
        settingsPage.style.pointerEvents = '';
        // 副导航栏滑入
        slideInSubSidebar(settingsPage);
      } else if (pageName === 'resource') {
        resourcePage.classList.add('active');
        resourcePage.style.opacity = '1';
        resourcePage.style.pointerEvents = '';
        // 副导航栏滑入
        slideInSubSidebar(resourcePage);
      }
    }, 200); // 缩短延迟到200ms
  }

  // 主界面卡片滑入动画
  function slideInHomeCards() {
    const versionCard = document.querySelector('.version-card');
    const startGameBtn = document.querySelector('.start-game-btn');
    const accountCard = document.querySelector('.account-card');

    if (versionCard) {
      versionCard.classList.remove('slide-in', 'slide-out');
      void versionCard.offsetWidth; // 强制重排
      versionCard.classList.add('slide-in');
    }
    if (startGameBtn) {
      startGameBtn.classList.remove('slide-in', 'slide-out');
      void startGameBtn.offsetWidth;
      startGameBtn.classList.add('slide-in');
    }
    if (accountCard) {
      accountCard.classList.remove('slide-in', 'slide-out');
      void accountCard.offsetWidth;
      accountCard.classList.add('slide-in');
    }
  }

  // 主界面卡片滑出动画
  function slideOutHomeCards() {
    const versionCard = document.querySelector('.version-card');
    const startGameBtn = document.querySelector('.start-game-btn');
    const accountCard = document.querySelector('.account-card');

    if (versionCard) {
      versionCard.classList.remove('slide-in');
      versionCard.classList.add('slide-out');
    }
    if (startGameBtn) {
      startGameBtn.classList.remove('slide-in');
      startGameBtn.classList.add('slide-out');
    }
    if (accountCard) {
      accountCard.classList.remove('slide-in');
      accountCard.classList.add('slide-out');
    }
  }

  // 副导航栏滑入动画
  function slideInSubSidebar(page) {
    const subSidebar = page.querySelector('.sub-sidebar');
    if (subSidebar) {
      subSidebar.classList.remove('slide-in', 'slide-out');
      void subSidebar.offsetWidth; // 强制重排
      subSidebar.classList.add('slide-in');
    }
  }

  // 副导航栏滑出动画（所有页面的副导航栏向左滑出）
  function slideOutSubSidebars() {
    const allSubSidebars = document.querySelectorAll('.sub-sidebar');
    allSubSidebars.forEach(subSidebar => {
      if (subSidebar.classList.contains('slide-in')) {
        subSidebar.classList.remove('slide-in');
        subSidebar.classList.add('slide-out');
      }
    });
  }

  // 侧边栏图标点击
  sidebarIcons.forEach(icon => {
    icon.addEventListener('click', () => {
      sidebarIcons.forEach(i => i.classList.remove('active'));
      icon.classList.add('active');

      const tab = icon.dataset.tab;
      switchPage(tab);
    });
  });

  // ---- 副导航栏点击事件 ----
  const subSidebarItems = document.querySelectorAll('#subSidebar .sub-sidebar-item');
  const downloadContent = document.getElementById('downloadContent');
  let activeCategory = null;

  // 下载内容区的视图代际：每次切换分类/详情页都会+1。
  // 命令在后台线程执行（不再阻塞UI），响应回来时用户可能已经切走了，
  // 异步回调必须先校验代际，否则旧响应会把当前视图覆盖掉。
  let downloadViewSeq = 0;

  function loadDownloadContent(category) {
    if (category === 'game') {
      loadVersionList();
    } else if (category === 'mods') {
      loadModBrowser();
    } else {
      downloadViewSeq++;
      downloadContent.innerHTML = `
        <div class="download-loading">
          <div class="download-spinner"></div>
          <p>选择分类查看内容</p>
        </div>
      `;
    }
  }

  // ==================== Mod浏览器逻辑 ====================
  let modSearchState = {
    query: '',
    versions: [],
    loaders: [],
    categories: [],
    index: 'relevance',
    offset: 0,
    limit: 20,
    hits: [],
    totalHits: 0
  };

  function loadModBrowser() {
    const view = ++downloadViewSeq;
    modSearchState = { query: '', versions: [], loaders: [], categories: [], index: 'relevance', offset: 0, limit: 20, hits: [], totalHits: 0 };

    downloadContent.innerHTML = `
      <div class="mod-browser">
        <div class="mod-search-bar">
          <input type="text" class="mod-search-input" id="modSearchInput" placeholder="搜索模组名称...">
          <select class="mod-filter-select" id="modVersionFilter"><option value="">游戏版本</option></select>
          <select class="mod-filter-select" id="modLoaderFilter"><option value="">加载器</option></select>
          <select class="mod-filter-select" id="modCategoryFilter"><option value="">功能分类</option></select>
          <button class="mod-search-btn" id="modSearchBtn">搜索</button>
          <button class="mod-reset-btn" id="modResetBtn">重置</button>
        </div>
        <div class="mod-results" id="modResults">
          <div class="mod-loading"><div class="download-spinner"></div><p>正在加载...</p></div>
        </div>
      </div>
    `;

    const searchInput = document.getElementById('modSearchInput');
    const versionFilter = document.getElementById('modVersionFilter');
    const loaderFilter = document.getElementById('modLoaderFilter');
    const categoryFilter = document.getElementById('modCategoryFilter');
    const searchBtn = document.getElementById('modSearchBtn');
    const resetBtn = document.getElementById('modResetBtn');
    const resultsDiv = document.getElementById('modResults');

    // 通过后端获取筛选选项
    tauri.invoke('modrinth.getTags').then((result) => {
      if (view !== downloadViewSeq) return;
      if (result && result.ok) {
        if (Array.isArray(result.versions)) {
          result.versions.forEach(v => {
            const o = document.createElement('option');
            o.value = v; o.textContent = v;
            versionFilter.appendChild(o);
          });
        }
        if (Array.isArray(result.loaders)) {
          result.loaders.forEach(l => {
            const o = document.createElement('option');
            o.value = l.value; o.textContent = l.label.charAt(0).toUpperCase() + l.label.slice(1);
            loaderFilter.appendChild(o);
          });
        }
        if (Array.isArray(result.categories)) {
          result.categories.forEach(c => {
            const o = document.createElement('option');
            o.value = c.value; o.textContent = c.label;
            categoryFilter.appendChild(o);
          });
        }
        doModSearch();
      } else {
        resultsDiv.innerHTML = '<div class="mod-empty"><p>加载筛选选项失败</p></div>';
      }
    }).catch(() => {
      if (view !== downloadViewSeq) return;
      resultsDiv.innerHTML = '<div class="mod-empty"><p>加载筛选选项失败</p></div>';
    });

    searchBtn.addEventListener('click', () => {
      modSearchState.query = searchInput.value.trim();
      modSearchState.offset = 0;
      modSearchState.hits = [];
      doModSearch();
    });

    searchInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') searchBtn.click();
    });

    resetBtn.addEventListener('click', () => {
      searchInput.value = '';
      versionFilter.value = '';
      loaderFilter.value = '';
      categoryFilter.value = '';
      modSearchState = { query: '', versions: [], loaders: [], categories: [], index: 'relevance', offset: 0, limit: 20, hits: [], totalHits: 0 };
      doModSearch();
    });

    versionFilter.addEventListener('change', () => {
      modSearchState.offset = 0; modSearchState.hits = []; doModSearch();
    });
    loaderFilter.addEventListener('change', () => {
      modSearchState.offset = 0; modSearchState.hits = []; doModSearch();
    });
    categoryFilter.addEventListener('change', () => {
      modSearchState.offset = 0; modSearchState.hits = []; doModSearch();
    });
  }

  function doModSearch() {
    const view = downloadViewSeq;
    const resultsDiv = document.getElementById('modResults');
    if (!resultsDiv) return;

    const versionFilter = document.getElementById('modVersionFilter');
    const loaderFilter = document.getElementById('modLoaderFilter');
    const categoryFilter = document.getElementById('modCategoryFilter');

    if (modSearchState.offset === 0) {
      resultsDiv.innerHTML = '<div class="mod-loading"><div class="download-spinner"></div><p>搜索中...</p></div>';
    }

    // 通过后端搜索
    tauri.invoke('modrinth.search', {
      query: modSearchState.query,
      index: modSearchState.index,
      offset: modSearchState.offset,
      limit: modSearchState.limit,
      versions: versionFilter ? versionFilter.value : '',
      loaders: loaderFilter ? loaderFilter.value : '',
      categories: categoryFilter ? categoryFilter.value : ''
    }).then((data) => {
      if (!resultsDiv || view !== downloadViewSeq) return;
      const hits = data.hits || [];
      modSearchState.totalHits = data.total_hits || 0;

      if (modSearchState.offset === 0) {
        modSearchState.hits = hits;
      } else {
        modSearchState.hits = modSearchState.hits.concat(hits);
      }

      renderModResults();
    }).catch(() => {
      if (modSearchState.offset === 0 && view === downloadViewSeq) {
        resultsDiv.innerHTML = '<div class="mod-empty"><p>搜索失败，请稍后重试</p></div>';
      }
    });
  }

  function renderModResults() {
    const resultsDiv = document.getElementById('modResults');
    if (!resultsDiv) return;
    resultsDiv.innerHTML = '';

    if (modSearchState.hits.length === 0) {
      resultsDiv.innerHTML = '<div class="mod-empty"><p>没有找到相关模组</p></div>';
      return;
    }

    modSearchState.hits.forEach(mod => {
      const card = document.createElement('div');
      card.className = 'mod-card';

      const categories = (mod.categories || []).slice(0, 3);
      const versions = mod.versions || [];
      const versionRange = versions.length > 2
        ? versions[versions.length - 1] + ' ~ ' + versions[0]
        : versions.join(', ') || '未知';

      const updateTime = mod.date_modified ? new Date(mod.date_modified).toLocaleDateString('zh-CN') : '';
      const downloads = mod.downloads >= 1000000
        ? (mod.downloads / 1000000).toFixed(1) + 'M'
        : mod.downloads >= 1000
          ? (mod.downloads / 1000).toFixed(1) + 'K'
          : String(mod.downloads);

      card.innerHTML = `
        <img class="mod-card-icon" src="${mod.icon_url || ''}" alt="" onerror="this.style.display='none'">
        <div class="mod-card-info">
          <div class="mod-card-header">
            <span class="mod-card-name">${mod.title || mod.slug}</span>
            <div class="mod-card-categories">
              ${categories.map(c => '<span class="mod-card-tag">' + c + '</span>').join('')}
            </div>
          </div>
          <div class="mod-card-desc">${mod.description || ''}</div>
          <div class="mod-card-meta">
            <span class="mod-card-meta-item">
              <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
              ${downloads}
            </span>
            <span class="mod-card-meta-item">
              <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="4" width="18" height="18" rx="2" ry="2"/><line x1="16" y1="2" x2="16" y2="6"/><line x1="8" y1="2" x2="8" y2="6"/><line x1="3" y1="10" x2="21" y2="10"/></svg>
              ${updateTime}
            </span>
            <span class="mod-card-meta-item">
              <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>
              ${versionRange}
            </span>
          </div>
        </div>
      `;

      card.addEventListener('click', () => {
        tauri.invoke('shell.open', { url: 'https://modrinth.com/project/' + mod.slug });
      });

      resultsDiv.appendChild(card);
    });

    // 加载更多按钮
    if (modSearchState.hits.length < modSearchState.totalHits) {
      const loadMoreBtn = document.createElement('button');
      loadMoreBtn.className = 'mod-load-more';
      loadMoreBtn.textContent = `加载更多 (${modSearchState.hits.length} / ${modSearchState.totalHits})`;
      loadMoreBtn.addEventListener('click', () => {
        modSearchState.offset += modSearchState.limit;
        doModSearch();
      });
      resultsDiv.appendChild(loadMoreBtn);
    }
  }

  function loadVersionList() {
    const view = ++downloadViewSeq;
    downloadContent.innerHTML = `
      <div class="download-loading">
        <div class="download-spinner"></div>
        <p>正在获取版本列表...</p>
      </div>
    `;
    tauri.invoke('version.list').then((result) => {
      if (view !== downloadViewSeq) return;
      if (!result || !result.ok) {
        downloadContent.innerHTML = `
          <div class="download-loading">
            <p>暂无可用下载</p>
          </div>
        `;
        return;
      }
      const groups = result.groups || {};
      const groupKeys = Object.keys(groups);
      if (groupKeys.length === 0) {
        downloadContent.innerHTML = `<div class="download-loading"><p>版本列表为空</p></div>`;
        return;
      }

      const typeOrder = ['release', 'snapshot', 'old_beta', 'old_alpha'];
      const typeLabelMap = {
        release: '正式版',
        snapshot: '快照版',
        old_beta: '旧版测试',
        old_alpha: '旧版内测'
      };
      groupKeys.sort((a, b) => {
        const ia = typeOrder.indexOf(a);
        const ib = typeOrder.indexOf(b);
        return (ia === -1 ? 100 : ia) - (ib === -1 ? 100 : ib);
      });

      const wrapper = document.createElement('div');
      wrapper.className = 'version-groups';

      const createVersionItem = (v) => {
        const item = document.createElement('button');
        item.type = 'button';
        item.className = 'version-item';
        const time = v.releaseTime ? v.releaseTime.split('T')[0] : '';
        item.innerHTML = `
          <div class="version-item-info">
            <span class="version-id">${v.id}</span>
            <span class="version-date">${time}</span>
          </div>
          <span class="version-type version-type-${v.type}">${typeLabelMap[v.type] || v.type}</span>
        `;
        item.addEventListener('click', () => {
          showDownloadInfo(v);
        });
        return item;
      };

      const createSection = (title, versions, defaultOpen) => {
        const section = document.createElement('div');
        section.className = 'version-section';
        const header = document.createElement('button');
        header.type = 'button';
        header.className = 'version-section-header' + (defaultOpen ? '' : ' collapsed');
        header.setAttribute('aria-expanded', String(defaultOpen));
        header.innerHTML = `
          <span class="version-section-arrow">&#9660;</span>
          <span class="version-section-title">${title}</span>
          <span class="version-section-count">${versions.length}</span>
        `;
        const body = document.createElement('div');
        body.className = 'version-section-body' + (defaultOpen ? ' open' : '');
        header.addEventListener('click', () => {
          const open = body.classList.toggle('open');
          header.classList.toggle('collapsed', !open);
          header.setAttribute('aria-expanded', String(open));
        });
        versions.forEach(v => body.appendChild(createVersionItem(v)));
        section.appendChild(header);
        section.appendChild(body);
        return section;
      };

      const latestItems = [];
      if (groups.release && groups.release.length > 0) latestItems.push(groups.release[0]);
      if (groups.snapshot && groups.snapshot.length > 0) latestItems.push(groups.snapshot[0]);
      if (latestItems.length > 0) wrapper.appendChild(createSection('最新版', latestItems, true));

      groupKeys.forEach(type => {
        const versions = groups[type];
        if (!Array.isArray(versions) || versions.length === 0) return;
        wrapper.appendChild(createSection(typeLabelMap[type] || type, versions, false));
      });

      downloadContent.innerHTML = '';
      downloadContent.appendChild(wrapper);
    }).catch(() => {
      if (view !== downloadViewSeq) return;
      downloadContent.innerHTML = `
        <div class="download-loading">
          <p>获取版本列表失败</p>
        </div>
      `;
    });
  }

  function showDownloadInfo(v) {
    // 进入版本详情也是新视图，使在途的列表/搜索响应失效
    downloadViewSeq++;
    const phaseLabelMap = { assets: '资源文件', libraries: '库文件', client: '客户端' };

    downloadContent.innerHTML = '';
    const infoPage = document.createElement('div');
    infoPage.className = 'download-info-page';

    const topBar = document.createElement('div');
    topBar.className = 'download-info-topbar';
    topBar.innerHTML = `<span class="download-info-back">&#9664; 返回</span>`;
    topBar.querySelector('.download-info-back').addEventListener('click', () => loadVersionList());

    // 版本命名
    const nameRow = document.createElement('div');
    nameRow.className = 'download-info-row';
    nameRow.innerHTML = `
      <label class="download-info-label">版本命名</label>
      <input type="text" class="download-info-input" value="${v.id}">
    `;
    const nameInput = nameRow.querySelector('.download-info-input');

    // 模组加载器选择区域
    const loaderSection = document.createElement('div');
    loaderSection.className = 'download-info-options';
    loaderSection.innerHTML = `
      <div class="download-info-option-label" style="font-weight:600;margin-bottom:4px;">安装模组加载器（可选）</div>
    `;

    let selectedLoaderType = '';
    let selectedLoaderVersion = '';
    let selectedDownloadUrl = '';

    // 加载器类型选择
    const loaderTypeRow = document.createElement('div');
    loaderTypeRow.className = 'download-info-option';
    loaderTypeRow.style.cursor = 'default';
    loaderTypeRow.innerHTML = `
      <span class="download-info-option-label">加载器类型</span>
      <select class="modloader-type-select" style="padding:4px 8px;border:1px solid rgba(0,0,0,0.1);border-radius:6px;font-size:13px;background:rgba(255,255,255,0.5);outline:none;">
        <option value="">不安装</option>
        <option value="forge">Forge</option>
        <option value="fabric">Fabric</option>
        <option value="quilt">Quilt</option>
        <option value="neoforge">NeoForge</option>
      </select>
    `;
    const loaderTypeSelect = loaderTypeRow.querySelector('.modloader-type-select');

    // 加载器版本选择
    const loaderVersionRow = document.createElement('div');
    loaderVersionRow.className = 'download-info-option';
    loaderVersionRow.style.cursor = 'default';
    loaderVersionRow.style.display = 'none';
    loaderVersionRow.innerHTML = `
      <span class="download-info-option-label">加载器版本</span>
      <select class="modloader-version-select" style="padding:4px 8px;border:1px solid rgba(0,0,0,0.1);border-radius:6px;font-size:13px;background:rgba(255,255,255,0.5);outline:none;min-width:120px;">
        <option value="">请选择</option>
      </select>
    `;
    const loaderVersionSelect = loaderVersionRow.querySelector('.modloader-version-select');

    // 加载器版本列表缓存
    let loaderVersionsCache = {};

    // 更新版本命名（强制设置）
    function updateVersionName() {
      if (selectedLoaderType && selectedLoaderVersion) {
        nameInput.value = v.id + '-' + selectedLoaderType + '-' + selectedLoaderVersion;
      } else {
        nameInput.value = v.id;
      }
      nameInput.dispatchEvent(new Event('input'));
    }

    // 加载器类型变化
    loaderTypeSelect.addEventListener('change', () => {
      selectedLoaderType = loaderTypeSelect.value;
      selectedLoaderVersion = '';
      selectedDownloadUrl = '';

      if (!selectedLoaderType) {
        loaderVersionRow.style.display = 'none';
        updateVersionName();
        return;
      }

      loaderVersionRow.style.display = 'flex';
      const versionSelect = loaderVersionRow.querySelector('.modloader-version-select');
      versionSelect.innerHTML = '<option value="">加载中...</option>';

      // 检查缓存
      const cacheKey = selectedLoaderType + '_' + v.id;
      if (loaderVersionsCache[cacheKey]) {
        renderLoaderVersions(loaderVersionsCache[cacheKey]);
        return;
      }

      // 获取加载器版本列表
      const apiMap = {
        forge: 'modloader.getForgeVersions',
        fabric: 'modloader.getFabricVersions',
        quilt: 'modloader.getQuiltVersions',
        neoforge: 'modloader.getNeoForgeVersions'
      };

      tauri.invoke(apiMap[selectedLoaderType], { gameVersion: v.id }).then((result) => {
        if (result && result.ok && result.versions) {
          loaderVersionsCache[cacheKey] = result.versions;
          renderLoaderVersions(result.versions);
        } else {
          versionSelect.innerHTML = '<option value="">无可用版本</option>';
        }
      }).catch(() => {
        versionSelect.innerHTML = '<option value="">获取失败</option>';
      });
    });

    function renderLoaderVersions(versions) {
      const versionSelect = loaderVersionRow.querySelector('.modloader-version-select');
      versionSelect.innerHTML = '<option value="">请选择</option>';
      versions.forEach(ver => {
        const option = document.createElement('option');
        option.value = ver.version;
        option.textContent = ver.version;
        option.dataset.downloadUrl = ver.downloadUrl;
        versionSelect.appendChild(option);
      });

      // 自动选择最新版本（第一个）
      if (versions.length > 0) {
        versionSelect.selectedIndex = 1;
        selectedLoaderVersion = versions[0].version;
        selectedDownloadUrl = versions[0].downloadUrl;
        updateVersionName();
      }
    }

    // 加载器版本变化
    loaderVersionSelect.addEventListener('change', () => {
      selectedLoaderVersion = loaderVersionSelect.value;
      const selectedOption = loaderVersionSelect.options[loaderVersionSelect.selectedIndex];
      selectedDownloadUrl = selectedOption ? (selectedOption.dataset.downloadUrl || '') : '';
      updateVersionName();
    });

    loaderSection.appendChild(loaderTypeRow);
    loaderSection.appendChild(loaderVersionRow);

    // 进度条
    const progressArea = document.createElement('div');
    progressArea.className = 'download-progress-area';
    progressArea.style.display = 'none';
    progressArea.innerHTML = `
      <div class="download-progress-bar">
        <div class="download-progress-fill"></div>
      </div>
      <div class="download-progress-info">
        <span class="download-progress-text">准备中...</span>
        <span class="download-progress-percent">0%</span>
      </div>
    `;
    const progressFill = progressArea.querySelector('.download-progress-fill');
    const progressText = progressArea.querySelector('.download-progress-text');
    const progressPercent = progressArea.querySelector('.download-progress-percent');

    const updateProgress = (progress) => {
      const totalBytes = progress.totalBytes || 0;
      const downloadedBytes = progress.downloadedBytes || 0;
      const percent = totalBytes > 0 ? Math.round((downloadedBytes / totalBytes) * 100) : 0;
      progressFill.style.transform = 'scaleX(' + (percent / 100) + ')';
      progressPercent.textContent = percent + '%';
      const phase = progress.phase || '';
      const label = phaseLabelMap[phase] || phase;
      const done = progress.completedFiles || 0;
      const total = progress.totalFiles || 0;
      const failed = progress.failedFiles || 0;
      let text = label;
      if (total > 0) text += ' ' + done + '/' + total;
      if (failed > 0) text += ' (失败 ' + failed + ')';
      progressText.textContent = text;
    };

    const pollResult = (phase, timeoutMs = 600000, onProgress) => {
      return new Promise((resolve, reject) => {
        const startTime = Date.now();
        const poll = () => {
          if (Date.now() - startTime > timeoutMs) { reject(new Error('Timeout')); return; }
          tauri.invoke('download.poll').then((resp) => {
            if (resp && resp.progress && onProgress) onProgress(resp.progress);
            if (resp && resp.hasResult && resp.data && resp.data.phase === phase) {
              resolve(resp.data.result);
            } else {
              setTimeout(poll, 300);
            }
          }).catch(() => { setTimeout(poll, 500); });
        };
        poll();
      });
    };

    const pollModLoaderResult = (timeoutMs = 600000) => {
      return new Promise((resolve, reject) => {
        const startTime = Date.now();
        const poll = () => {
          if (Date.now() - startTime > timeoutMs) { reject(new Error('Timeout')); return; }
          tauri.invoke('modloader.poll').then((resp) => {
            if (resp && resp.hasResult) {
              resolve(resp);
            } else {
              setTimeout(poll, 500);
            }
          }).catch(() => { setTimeout(poll, 1000); });
        };
        poll();
      });
    };

    // 下载按钮
    const btnRow = document.createElement('div');
    btnRow.className = 'download-info-btn-row';
    const startBtn = document.createElement('button');
    startBtn.className = 'download-info-start-btn';
    startBtn.textContent = '开始下载';

    startBtn.addEventListener('click', () => {
      const versionName = nameInput.value.trim() || v.id;
      startBtn.disabled = true;
      startBtn.textContent = '正在获取版本信息...';
      progressArea.style.display = 'block';

      let versionResult = null;
      const allFailedFiles = [];

      tauri.invoke('version.download', { url: v.url, versionId: versionName, actualVersionId: v.id });

      pollResult('version', 600000, updateProgress).then((result) => {
        if (!result || !result.ok) throw new Error(result.error || '版本信息获取失败');
        versionResult = result;
        if (result.assetIndex && result.assetIndex.url) {
          startBtn.textContent = '正在下载资源文件...';
          tauri.invoke('assets.download', {
            assetIndexUrl: result.assetIndex.url,
            assetIndexId: result.assetIndex.id,
            profileName: versionName
          });
          return pollResult('assets', 600000, updateProgress);
        }
        return null;
      }).then((assetsResult) => {
        if (assetsResult && assetsResult.failedFiles) allFailedFiles.push(...assetsResult.failedFiles);
        if (versionResult && versionResult.libraries && versionResult.libraries.length > 0) {
          startBtn.textContent = '正在下载库文件...';
          tauri.invoke('libraries.download', { libraries: versionResult.libraries });
          return pollResult('libraries', 600000, updateProgress);
        }
        return null;
      }).then((libsResult) => {
        if (libsResult && libsResult.failedFiles) allFailedFiles.push(...libsResult.failedFiles);
        if (versionResult && versionResult.clientDownload && versionResult.clientDownload.url) {
          startBtn.textContent = '正在下载客户端...';
          tauri.invoke('client.download', {
            url: versionResult.clientDownload.url,
            sha1: versionResult.clientDownload.sha1 || '',
            profileName: versionName,
            actualVersionId: v.id
          });
          return pollResult('client', 600000, updateProgress);
        }
        return null;
      }).then((clientResult) => {
        if (clientResult && clientResult.failedFiles) allFailedFiles.push(...clientResult.failedFiles);
        if (versionResult && versionResult.mainClass) {
          return tauri.invoke('version.save', {
            profileName: versionName,
            actualVersionId: v.id,
            mainClass: versionResult.mainClass
          });
        }
        return null;
      }).then(() => {
        // 如果选择了模组加载器，开始安装
        if (selectedLoaderType && selectedLoaderVersion && selectedDownloadUrl) {
          startBtn.textContent = '正在安装' + selectedLoaderType + '...';
          progressFill.style.transform = 'scaleX(1)';
          progressPercent.textContent = '100%';
          progressText.textContent = '安装模组加载器中...';

          tauri.invoke('modloader.install', {
            loaderType: selectedLoaderType,
            loaderVersion: selectedLoaderVersion,
            downloadUrl: selectedDownloadUrl,
            gameVersion: v.id,
            profileName: nameInput.value.trim() || v.id
          });

          return pollModLoaderResult(600000);
        }
        return null;
      }).then((loaderResult) => {
        // 如果安装了模组加载器且返回了库列表，补下载Forge依赖
        if (loaderResult && loaderResult.ok && loaderResult.libraries && loaderResult.libraries.length > 0) {
          startBtn.textContent = '正在下载' + (selectedLoaderType || '模组加载器') + '依赖...';
          progressArea.style.display = 'block';
          progressText.textContent = '下载模组加载器依赖库...';
          tauri.invoke('libraries.download', { libraries: loaderResult.libraries });
          return pollResult('libraries', 600000, updateProgress).then((forgeLibsResult) => {
            if (forgeLibsResult && forgeLibsResult.failedFiles) allFailedFiles.push(...forgeLibsResult.failedFiles);
            return loaderResult;
          });
        }
        return loaderResult;
      }).then((loaderResult) => {
        progressArea.style.display = 'none';
        if (loaderResult && !loaderResult.ok) {
          startBtn.textContent = '加载器安装失败';
          startBtn.classList.add('is-error');
          alert(loaderResult.error || '模组加载器安装失败');
          setTimeout(() => {
            startBtn.textContent = '开始下载';
            startBtn.classList.remove('is-error');
            startBtn.disabled = false;
          }, 2000);
          return;
        }

        if (allFailedFiles.length > 0) {
          startBtn.textContent = '下载完成（部分失败）';
          startBtn.classList.add('is-warn');
          const failedList = document.createElement('div');
          failedList.className = 'download-failed-list';
          failedList.innerHTML = `<div class="download-failed-title">以下 ${allFailedFiles.length} 个文件下载失败：</div>`;
          allFailedFiles.forEach(f => {
            const item = document.createElement('div');
            item.className = 'download-failed-item';
            item.innerHTML = `
              <span class="download-failed-name" title="${f.url}">${f.name}</span>
              <span class="download-failed-error">${f.error}</span>
            `;
            failedList.appendChild(item);
          });
          const retryBtn = document.createElement('button');
          retryBtn.className = 'download-retry-btn';
          retryBtn.textContent = '重新下载失败文件';
          retryBtn.addEventListener('click', () => {
            failedList.remove();
            retryBtn.remove();
            startBtn.classList.remove('is-warn');
            startBtn.disabled = false;
            startBtn.textContent = '开始下载';
            progressArea.style.display = 'none';
          });
          failedList.appendChild(retryBtn);
          btnRow.parentNode.insertBefore(failedList, btnRow.nextSibling);
        } else {
          startBtn.textContent = '下载完成';
          startBtn.classList.remove('is-error', 'is-warn');
          tauri.invoke('version.select', { profileName: nameInput.value.trim() || v.id })
            .then(() => loadVersionCard());
        }
      }).catch((err) => {
        startBtn.disabled = false;
        startBtn.textContent = '开始下载';
        progressArea.style.display = 'none';
        alert(err.message || '网络请求失败');
      });
    });
    btnRow.appendChild(startBtn);

    infoPage.appendChild(topBar);
    infoPage.appendChild(nameRow);
    infoPage.appendChild(loaderSection);
    infoPage.appendChild(progressArea);
    infoPage.appendChild(btnRow);
    downloadContent.appendChild(infoPage);
  }

  subSidebarItems.forEach(item => {
    item.addEventListener('click', () => {
      subSidebarItems.forEach(i => i.classList.remove('active'));
      item.classList.add('active');
      activeCategory = item.dataset.category;
      loadDownloadContent(activeCategory);
    });
  });

  // ==================== 设置页逻辑 ====================
  const settingsContent = document.getElementById('settingsContent');
  const settingsSubSidebarItems = document.querySelectorAll('#settingsSubSidebar .sub-sidebar-item');
  let activeSettingsCategory = 'general';

  function loadSettingsContent(category) {
    if (category === 'general') loadGeneralSettings();
    else if (category === 'version') loadDownloadSettings();
    else if (category === 'ignore') loadIgnoreSettings();
    else if (category === 'account') loadAccountSettings();
  }

  settingsSubSidebarItems.forEach(item => {
    item.addEventListener('click', () => {
      settingsSubSidebarItems.forEach(i => i.classList.remove('active'));
      item.classList.add('active');
      activeSettingsCategory = item.dataset.category;
      loadSettingsContent(activeSettingsCategory);
    });
  });

  function loadGeneralSettings() {
    settingsContent.innerHTML = '';
    const wrapper = document.createElement('div');
    wrapper.className = 'settings-section';

    tauri.invoke('config.getThreads').then((result) => {
      const currentThreads = (result && result.ok) ? result.threads : 64;

      const title1 = document.createElement('div');
      title1.className = 'settings-section-title';
      title1.textContent = '启动器设置';
      wrapper.appendChild(title1);

      const langRow = document.createElement('div');
      langRow.className = 'settings-item';
      langRow.innerHTML = `
        <span class="settings-item-label">语言</span>
        <span class="settings-item-value">简体中文</span>
      `;
      wrapper.appendChild(langRow);

      const proxyRow = document.createElement('div');
      proxyRow.className = 'settings-item';
      proxyRow.innerHTML = `
        <span class="settings-item-label">代理设置</span>
        <span class="settings-item-value">无</span>
      `;
      wrapper.appendChild(proxyRow);

      // 消息测试按钮（调试用）
      const testRow = document.createElement('div');
      testRow.className = 'settings-item';
      testRow.style.cursor = 'pointer';
      testRow.innerHTML = `
        <span class="settings-item-label">消息测试</span>
        <span class="settings-item-value">点击测试</span>
      `;
      testRow.addEventListener('click', () => {
        // 显示测试消息，2秒后消失
        showNotification({
          title: '测试消息',
          content: '这是一条测试消息，2秒后将自动消失。',
          type: 'info',
          duration: 2000
        });
      });
      wrapper.appendChild(testRow);

      const title2 = document.createElement('div');
      title2.className = 'settings-section-title';
      title2.textContent = 'Java 配置';
      wrapper.appendChild(title2);

      const javaRow = document.createElement('div');
      javaRow.className = 'settings-item';
      javaRow.innerHTML = `
        <span class="settings-item-label">Java 路径</span>
        <span class="settings-item-value">自动检测</span>
        <span class="settings-item-arrow">&#9656;</span>
      `;
      javaRow.addEventListener('click', () => showJavaSelector());
      wrapper.appendChild(javaRow);

      settingsContent.appendChild(wrapper);
    });
  }

  function loadDownloadSettings() {
    settingsContent.innerHTML = `
      <div class="download-loading">
        <div class="download-spinner"></div>
        <p>加载中...</p>
      </div>
    `;

    const DEFAULT_SOURCES = [
      { id: 'mirror_first', name: '镜像优先，官方兜底' },
      { id: 'official', name: '全部官方' },
      { id: 'mirror_only', name: '全部镜像' }
    ];
    // 旧版本遗留值归一化：bmclapi→镜像优先，mcbbs→全部官方
    const LEGACY_SOURCE = { bmclapi: 'mirror_first', mcbbs: 'official' };
    const normalizeSource = v => LEGACY_SOURCE[v] || v;

    Promise.all([
      tauri.invoke('config.getDownloadSources').catch(() => null),
      tauri.invoke('config.getDownloadSettings')
    ]).then(([sourcesResult, result]) => {
      const settings = (result && result.ok && result.settings) ? result.settings : {
        source: 'mirror_first', versionListSource: 'mirror_first', maxThreads: 64, speedLimit: -1
      };
      // 归一化历史值，避免下拉框匹配不到任何选项而显示失真
      settings.source = normalizeSource(settings.source);
      settings.versionListSource = normalizeSource(settings.versionListSource);
      // 与后端 config.getDownloadSources 保持同步，避免前后端选项不一致
      const sources = (sourcesResult && Array.isArray(sourcesResult.sources) && sourcesResult.sources.length)
        ? sourcesResult.sources
        : DEFAULT_SOURCES;

      const wrapper = document.createElement('div');
      wrapper.className = 'settings-section';

      const title1 = document.createElement('div');
      title1.className = 'settings-section-title';
      title1.textContent = '下载设置';
      wrapper.appendChild(title1);

      // 文件下载源 / 版本列表源 共用同一组下载策略
      const buildSourceRow = (label, currentValue, onPick) => {
        const row = document.createElement('div');
        row.className = 'select-row';
        row.innerHTML = `
        <span class="select-row-label">${label}</span>
      `;
        const select = document.createElement('select');
        sources.forEach(opt => {
          const o = document.createElement('option');
          o.value = opt.id;
          o.textContent = opt.name;
          if (opt.id === currentValue) o.selected = true;
          select.appendChild(o);
        });
        select.addEventListener('change', () => {
          onPick(select.value);
          tauri.invoke('config.setDownloadSettings', { settings });
        });
        row.appendChild(select);
        return row;
      };

      wrapper.appendChild(buildSourceRow('文件下载源', settings.source, v => { settings.source = v; }));
      wrapper.appendChild(buildSourceRow('版本列表源', settings.versionListSource, v => { settings.versionListSource = v; }));

      // 最大线程数滑块
      const threadSlider = document.createElement('div');
      threadSlider.className = 'slider-row';
      const currentThreads = settings.maxThreads || 64;
      threadSlider.innerHTML = `
        <div class="slider-row-header">
          <span class="slider-row-label">最大线程数</span>
          <span class="slider-row-value">${currentThreads}</span>
        </div>
        <input type="range" min="1" max="256" value="${currentThreads}">
        <div class="slider-warning">下载线程过高可能导致卡顿</div>
      `;
      const threadInput = threadSlider.querySelector('input[type="range"]');
      const threadValue = threadSlider.querySelector('.slider-row-value');
      threadInput.addEventListener('input', () => {
        const val = parseInt(threadInput.value);
        threadValue.textContent = val;
        settings.maxThreads = val;
        tauri.invoke('config.setThreads', { threads: val });
        tauri.invoke('config.setDownloadSettings', { settings });
      });
      wrapper.appendChild(threadSlider);

      // 速度限制滑块
      const speedSlider = document.createElement('div');
      speedSlider.className = 'slider-row';
      const currentSpeed = settings.speedLimit != null ? settings.speedLimit : -1;
      const displaySpeed = currentSpeed < 0 ? '无限制' : (currentSpeed >= 1024 * 1024 ? (currentSpeed / (1024 * 1024)).toFixed(0) + ' MB/s' : currentSpeed + ' KB/s');
      speedSlider.innerHTML = `
        <div class="slider-row-header">
          <span class="slider-row-label">速度限制</span>
          <span class="slider-row-value">${displaySpeed}</span>
        </div>
        <input type="range" min="0" max="20" value="${currentSpeed < 0 ? 20 : Math.round(currentSpeed / (1024 * 1024))}">
      `;
      const speedInput = speedSlider.querySelector('input[type="range"]');
      const speedValue = speedSlider.querySelector('.slider-row-value');
      speedInput.addEventListener('input', () => {
        const val = parseInt(speedInput.value);
        if (val >= 20) {
          speedValue.textContent = '无限制';
          settings.speedLimit = -1;
        } else if (val === 0) {
          speedValue.textContent = '0 MB/s';
          settings.speedLimit = 0;
        } else {
          speedValue.textContent = val + ' MB/s';
          settings.speedLimit = val * 1024 * 1024;
        }
        tauri.invoke('config.setDownloadSettings', { settings });
      });
      wrapper.appendChild(speedSlider);

      settingsContent.innerHTML = '';
      settingsContent.appendChild(wrapper);
    }).catch(() => {
      settingsContent.innerHTML = `<div class="settings-section"><p style="color:#fff;">加载失败</p></div>`;
    });
  }

  function showJavaSelector() {
    settingsContent.innerHTML = `
      <div class="download-loading">
        <div class="download-spinner"></div>
        <p>正在扫描 Java...</p>
      </div>
    `;

    tauri.invoke('java.get').then((currentResult) => {
      const currentPath = (currentResult && currentResult.ok) ? currentResult.path : '';

      tauri.invoke('java.scan').then((result) => {
        if (!result || !result.ok || !result.javas || result.javas.length === 0) {
          settingsContent.innerHTML = `
            <div class="settings-section">
              <div class="settings-section-title">Java 路径</div>
              <div class="settings-item">
                <span class="settings-item-label">未找到 Java</span>
                <span class="settings-item-value">请手动安装后重试</span>
              </div>
            </div>
          `;
          return;
        }

        const versionMap = {};
        result.javas.forEach(java => {
          const ver = java.version || java.path;
          if (!versionMap[ver]) versionMap[ver] = java;
        });
        const uniqueJavas = Object.values(versionMap);

        const wrapper = document.createElement('div');
        wrapper.className = 'settings-section';

        const backRow = document.createElement('div');
        backRow.className = 'settings-item';
        backRow.style.cursor = 'pointer';
        backRow.innerHTML = `<span class="settings-item-label">&#9664; 返回</span>`;
        backRow.addEventListener('click', () => loadGeneralSettings());
        wrapper.appendChild(backRow);

        const title = document.createElement('div');
        title.className = 'settings-section-title';
        title.textContent = `检测到 ${uniqueJavas.length} 个 Java`;
        wrapper.appendChild(title);

        uniqueJavas.forEach(java => {
          const item = document.createElement('div');
          item.className = 'settings-item';
          const majorVersion = java.version ? java.version.split('.')[0] : '?';
          const displayName = java.version ? `Java ${majorVersion} (${java.version})` : '未知版本';
          const isSelected = java.path === currentPath;
          item.innerHTML = `
            <span class="settings-item-label">${displayName}</span>
            <span class="settings-item-value">${isSelected ? '已选择' : java.path}</span>
          `;
          if (isSelected) item.classList.add('is-selected');
          item.addEventListener('click', () => {
            tauri.invoke('java.select', { path: java.path }).then(() => showJavaSelector());
          });
          wrapper.appendChild(item);
        });

        settingsContent.innerHTML = '';
        settingsContent.appendChild(wrapper);
      });
    });
  }

  // ==================== 资源管理页逻辑 ====================
  const resourceContent = document.getElementById('resourceContent');
  const resourceSubSidebarItems = document.querySelectorAll('#resourceSubSidebar .sub-sidebar-item');
  let activeResourceCategory = 'versions';

  function loadResourceContent(category) {
    if (category === 'versions') loadResourceVersions();
  }

  resourceSubSidebarItems.forEach(item => {
    item.addEventListener('click', () => {
      resourceSubSidebarItems.forEach(i => i.classList.remove('active'));
      item.classList.add('active');
      activeResourceCategory = item.dataset.category;
      loadResourceContent(activeResourceCategory);
    });
  });

  function loadResourceVersions() {
    resourceContent.innerHTML = `
      <div class="download-loading">
        <div class="download-spinner"></div>
        <p>加载中...</p>
      </div>
    `;

    tauri.invoke('version.getInstalled').then((result) => {
      const wrapper = document.createElement('div');
      wrapper.className = 'settings-section';

      const title = document.createElement('div');
      title.className = 'settings-section-title';
      title.textContent = '已下载版本';
      wrapper.appendChild(title);

      if (result && result.ok && result.versions && result.versions.length > 0) {
        result.versions.forEach(v => {
          const item = document.createElement('div');
          item.className = 'settings-item';
          item.innerHTML = `
            <span class="settings-item-label">${v.profileName}</span>
            <span class="settings-item-value">${v.actualVersionId || ''}</span>
            <span class="settings-item-arrow">&#9656;</span>
          `;
          item.addEventListener('click', () => showResourceVersionDetail(v));
          wrapper.appendChild(item);
        });
      } else {
        const empty = document.createElement('div');
        empty.className = 'settings-item';
        empty.innerHTML = `<span class="settings-item-label" style="color:#94a3b8;">暂无已下载版本</span>`;
        wrapper.appendChild(empty);
      }

      resourceContent.innerHTML = '';
      resourceContent.appendChild(wrapper);
    }).catch(() => {
      resourceContent.innerHTML = `<div class="settings-section"><p style="color:#fff;">加载失败</p></div>`;
    });
  }

  function showResourceVersionDetail(v) {
    resourceContent.innerHTML = '';
    const wrapper = document.createElement('div');
    wrapper.className = 'settings-section';

    const backRow = document.createElement('div');
    backRow.className = 'settings-item';
    backRow.style.cursor = 'pointer';
    backRow.innerHTML = `<span class="settings-item-label">&#9664; 返回</span>`;
    backRow.addEventListener('click', () => loadResourceVersions());
    wrapper.appendChild(backRow);

    const title = document.createElement('div');
    title.className = 'settings-section-title';
    title.textContent = v.profileName;
    wrapper.appendChild(title);

    const info = [
      { label: '版本名称', value: v.profileName },
      { label: '实际版本', value: v.actualVersionId || '未知' },
      { label: '主类', value: v.mainClass || '未知' }
    ];
    info.forEach(item => {
      const row = document.createElement('div');
      row.className = 'settings-item';
      row.innerHTML = `
        <span class="settings-item-label">${item.label}</span>
        <span class="settings-item-value">${item.value}</span>
      `;
      wrapper.appendChild(row);
    });

    const deleteRow = document.createElement('div');
    deleteRow.className = 'settings-item is-danger';
    deleteRow.innerHTML = `<span class="settings-item-label">删除此版本</span>`;
    deleteRow.addEventListener('click', () => {
      if (confirm(`确定要删除版本 ${v.profileName} 吗？`)) {
        tauri.invoke('version.delete', { profileName: v.profileName }).then((result) => {
          if (result && result.ok) {
            loadResourceVersions();
            loadVersionCard();
          } else {
            alert((result && result.error) || '删除失败');
          }
        });
      }
    });
    wrapper.appendChild(deleteRow);

    resourceContent.appendChild(wrapper);
  }

  function loadIgnoreSettings() {
    settingsContent.innerHTML = '';
    const wrapper = document.createElement('div');
    wrapper.className = 'settings-section';

    const title = document.createElement('div');
    title.className = 'settings-section-title';
    title.textContent = '忽略库列表';
    wrapper.appendChild(title);

    const desc = document.createElement('div');
    desc.className = 'settings-item';
    desc.style.flexDirection = 'column';
    desc.style.alignItems = 'stretch';
    desc.style.cursor = 'default';
    desc.innerHTML = `
      <span class="settings-item-label" style="color:#64748b;font-size:12px;line-height:1.5;">
        在忽略列表中的库文件将跳过下载和完整性检查，不会阻止游戏启动。
      </span>
    `;
    wrapper.appendChild(desc);

    const addRow = document.createElement('div');
    addRow.className = 'settings-item';
    addRow.style.flexDirection = 'column';
    addRow.style.alignItems = 'stretch';
    addRow.style.gap = '8px';
    addRow.style.cursor = 'default';
    addRow.innerHTML = `
      <div style="display:flex;gap:8px;align-items:center;">
        <input type="text" id="ignore-add-input" placeholder="输入库名称"
          style="flex:1;padding:6px 10px;border:1px solid rgba(0,0,0,0.1);border-radius:6px;font-size:13px;background:rgba(255,255,255,0.5);outline:none;">
        <button id="ignore-add-btn" style="padding:6px 14px;border:none;border-radius:6px;background:#3b82f6;color:#fff;font-size:13px;cursor:pointer;white-space:nowrap;">添加</button>
      </div>
    `;
    wrapper.appendChild(addRow);

    const listContainer = document.createElement('div');
    listContainer.id = 'ignore-list-container';
    wrapper.appendChild(listContainer);

    settingsContent.appendChild(wrapper);

    function refreshIgnoreList() {
      tauri.invoke('ignorelist.get').then((result) => {
        listContainer.innerHTML = '';
        if (result && result.ok && result.list && result.list.length > 0) {
          result.list.forEach(name => {
            const item = document.createElement('div');
            item.className = 'settings-item';
            item.style.justifyContent = 'space-between';
            item.innerHTML = `
              <span class="settings-item-label" style="font-size:13px;word-break:break-all;">${name}</span>
              <span class="settings-item-value" style="color:#ef4444;cursor:pointer;padding:2px 8px;border-radius:4px;background:rgba(239,68,68,0.1);white-space:nowrap;">移除</span>
            `;
            item.querySelector('.settings-item-value').addEventListener('click', (e) => {
              e.stopPropagation();
              tauri.invoke('ignorelist.remove', { name }).then(() => refreshIgnoreList());
            });
            listContainer.appendChild(item);
          });
        } else {
          const empty = document.createElement('div');
          empty.className = 'settings-item';
          empty.style.cursor = 'default';
          empty.innerHTML = `<span class="settings-item-label" style="color:#94a3b8;">暂无忽略项</span>`;
          listContainer.appendChild(empty);
        }
      });
    }

    refreshIgnoreList();

    const addBtn = settingsContent.querySelector('#ignore-add-btn');
    const addInput = settingsContent.querySelector('#ignore-add-input');
    addBtn.addEventListener('click', () => {
      const name = addInput.value.trim();
      if (!name) return;
      tauri.invoke('ignorelist.add', { name }).then(() => {
        addInput.value = '';
        refreshIgnoreList();
      });
    });
    addInput.addEventListener('keydown', (e) => { if (e.key === 'Enter') addBtn.click(); });
  }

  function loadAccountSettings() {
    settingsContent.innerHTML = '';
    const wrapper = document.createElement('div');
    wrapper.className = 'settings-section';

    const title = document.createElement('div');
    title.className = 'settings-section-title';
    title.textContent = '角色管理';
    wrapper.appendChild(title);

    tauri.invoke('account.read').then((config) => {
      if (config && config.accounts) {
        const loginIcon = '../resoures/icon/steve.png';
        config.accounts.forEach((id, i) => {
          const item = document.createElement('div');
          item.className = 'settings-item';
          if (id && id.length > 0) {
            item.innerHTML = `
              <span class="settings-item-label">${id}</span>
              <span class="settings-item-value">槽位 ${i + 1}</span>
              <span class="settings-item-arrow">&#9656;</span>
            `;
            item.addEventListener('click', () => showAccountDetail(i, id));
          } else {
            item.innerHTML = `
              <span class="settings-item-label" style="color:#94a3b8;">空槽位 ${i + 1}</span>
              <span class="settings-item-value">未登录</span>
            `;
          }
          wrapper.appendChild(item);
        });
      }
      settingsContent.appendChild(wrapper);
    });
  }

  function showAccountDetail(index, id) {
    settingsContent.innerHTML = '';
    const wrapper = document.createElement('div');
    wrapper.className = 'settings-section';

    const backRow = document.createElement('div');
    backRow.className = 'settings-item';
    backRow.style.cursor = 'pointer';
    backRow.innerHTML = `<span class="settings-item-label">&#9664; 返回</span>`;
    backRow.addEventListener('click', () => loadAccountSettings());
    wrapper.appendChild(backRow);

    const title = document.createElement('div');
    title.className = 'settings-section-title';
    title.textContent = '角色详情';
    wrapper.appendChild(title);

    const info = [
      { label: '角色名', value: id },
      { label: '槽位', value: String(index + 1) }
    ];
    info.forEach(item => {
      const row = document.createElement('div');
      row.className = 'settings-item';
      row.innerHTML = `
        <span class="settings-item-label">${item.label}</span>
        <span class="settings-item-value">${item.value}</span>
      `;
      wrapper.appendChild(row);
    });

    const deleteRow = document.createElement('div');
    deleteRow.className = 'settings-item is-danger';
    deleteRow.innerHTML = `<span class="settings-item-label">删除角色</span>`;
    deleteRow.addEventListener('click', () => {
      if (confirm(`确定要删除角色 ${id} 吗？`)) {
        tauri.invoke('account.write', { index, id: '', name: '' }).then(() => {
          loadAccountSettings();
          loadAccountCard();
        });
      }
    });
    wrapper.appendChild(deleteRow);

    settingsContent.appendChild(wrapper);
  }

  // ==================== 角色信息卡片逻辑 ====================
  const accountCard = document.getElementById('accountCard');
  const accountCardAvatar = document.getElementById('accountCardAvatar');
  const accountCardName = document.getElementById('accountCardName');
  const accountCardExpand = document.getElementById('accountCardExpand');
  const accountCardList = document.getElementById('accountCardList');
  const accountManageBtn = document.getElementById('accountManageBtn');
  const accountCardMain = document.getElementById('accountCardMain');
  let currentAccountIndex = -1;

  // 点击切换账户卡片展开（兼容触屏与键盘）
  accountCardMain.addEventListener('click', () => {
    const expanded = accountCard.classList.toggle('expanded');
    accountCardMain.setAttribute('aria-expanded', String(expanded));
  });

  // 加载账户信息到角色卡片
  function loadAccountCard() {
    tauri.invoke('account.read').then((config) => {
      if (config && config.accounts) {
        // 更新主卡片显示（显示第一个有账户的）
        let foundAccount = false;
        for (let i = 0; i < config.accounts.length; i++) {
          if (config.accounts[i] && config.accounts[i].length > 0) {
            accountCardAvatar.src = loginIcon;
            accountCardName.textContent = config.accounts[i];
            currentAccountIndex = i;
            foundAccount = true;
            break;
          }
        }
        if (!foundAccount) {
          accountCardAvatar.src = '../resoures/icon/nukownP.png';
          accountCardName.textContent = '未登录';
          currentAccountIndex = -1;
        }

        // 填充账户列表
        accountCardList.innerHTML = '';
        let hasAccounts = false;
        config.accounts.forEach((id, i) => {
          if (id && id.length > 0) {
            hasAccounts = true;
            const item = document.createElement('button');
            item.type = 'button';
            item.className = 'account-list-item';
            if (i === currentAccountIndex) {
              item.classList.add('active');
            }
            item.innerHTML = `
              <img class="account-list-item-avatar" src="${loginIcon}" alt="avatar">
              <span class="account-list-item-name">${id}</span>
            `;
            item.addEventListener('click', () => switchAccount(i, id));
            accountCardList.appendChild(item);
          }
        });

        if (!hasAccounts) {
          accountCardList.innerHTML = '<div class="account-list-empty">暂无已登录账户</div>';
        }
      }
    }).catch(() => {});
  }

  // 切换账户
  function switchAccount(index, name) {
    currentAccountIndex = index;
    accountCardAvatar.src = loginIcon;
    accountCardName.textContent = name;

    // 更新列表中的活动状态
    const items = accountCardList.querySelectorAll('.account-list-item');
    items.forEach((item, i) => {
      item.classList.toggle('active', i === index);
    });
  }

  // 管理账户按钮点击事件
  accountManageBtn.addEventListener('click', () => {
    // 淡出大厅层
    layerLobby.style.transition = 'opacity 0.4s ease';
    layerLobby.style.opacity = '0';

    // 400ms后隐藏大厅层，显示登录层
    setTimeout(() => {
      layerLobby.classList.remove('active');
      layerLobby.style.opacity = '';
      layerLobby.style.transition = '';

      // 重置登录按钮状态
      loginBtn.textContent = '登录';
      loginBtn.disabled = false;
      loginInput.value = '';

      // 清空所有账户数据，回到登录状态
      tauri.invoke('account.write', { index: 0, id: '', name: '' }).then(() => {
        return tauri.invoke('account.write', { index: 1, id: '', name: '' });
      }).then(() => {
        return tauri.invoke('account.write', { index: 2, id: '', name: '' });
      }).then(() => {
        showLayer(layerLogin);
      });
    }, 400);
  });

  // 初始化加载账户卡片
  loadAccountCard();

  // ==================== 版本选择卡片逻辑 ====================
  const versionCard = document.getElementById('versionCard');
  const versionCardName = document.getElementById('versionCardName');
  const versionCardList = document.getElementById('versionCardList');
  const versionCardMain = document.getElementById('versionCardMain');
  let currentVersionName = null;

  // 点击切换版本卡片展开（兼容触屏与键盘）
  versionCardMain.addEventListener('click', () => {
    const expanded = versionCard.classList.toggle('expanded');
    versionCardMain.setAttribute('aria-expanded', String(expanded));
  });

  // 加载版本信息
  function loadVersionCard() {
    tauri.invoke('version.getInstalled').then((result) => {
      if (result && result.ok && result.versions && result.versions.length > 0) {
        versionCardList.innerHTML = '';
        result.versions.forEach(v => {
          const item = document.createElement('button');
          item.type = 'button';
          item.className = 'version-list-item';
          item.innerHTML = `
            <img class="version-list-item-icon" src="../resoures/icon/grass_block_side.png" alt="version">
            <span class="version-list-item-name">${v.profileName}</span>
          `;
          item.addEventListener('click', () => switchVersion(v.profileName));
          versionCardList.appendChild(item);
        });

        // 加载已选版本
        tauri.invoke('version.getSelected').then((sel) => {
          if (sel && sel.ok && sel.profileName) {
            currentVersionName = sel.profileName;
            versionCardName.textContent = sel.profileName;
            updateVersionListActive();
          }
        }).catch(() => {});
      } else {
        versionCardName.textContent = '未下载版本';
        versionCardList.innerHTML = '<div class="version-list-empty">暂无已下载版本</div>';
      }
    }).catch(() => {});
  }

  // 切换版本
  function switchVersion(profileName) {
    currentVersionName = profileName;
    versionCardName.textContent = profileName;
    tauri.invoke('version.select', { profileName: profileName });
    updateVersionListActive();
  }

  // 更新版本列表活动状态
  function updateVersionListActive() {
    const items = versionCardList.querySelectorAll('.version-list-item');
    items.forEach((item, i) => {
      const name = item.querySelector('.version-list-item-name').textContent;
      item.classList.toggle('active', name === currentVersionName);
    });
  }

  // 初始化加载版本卡片
  loadVersionCard();

  // ==================== 开始游戏按钮逻辑 ====================
  const startGameBtn = document.getElementById('startGameBtn');
  let gameLaunchInProgress = false;

  // 监听游戏启动进度事件
  tauri.listen('game.launch.progress', (event) => {
    console.log('Game launch progress:', event);

    if (event.status === 'waiting_window') {
      startGameBtn.textContent = '等待游戏窗口出现';
    } else if (event.status === 'window_ready') {
      startGameBtn.textContent = '游戏已启动';
      gameLaunchInProgress = false;
      setTimeout(() => {
        startGameBtn.textContent = '开始游戏';
        startGameBtn.disabled = false;
      }, 2000);
    } else if (event.status === 'game_ended') {
      startGameBtn.textContent = '开始游戏';
      startGameBtn.disabled = false;
      gameLaunchInProgress = false;
    }
  });

  startGameBtn.addEventListener('click', () => {
    if (!currentVersionName) {
      alert('请先选择一个游戏版本');
      return;
    }

    if (gameLaunchInProgress) {
      return;
    }

    gameLaunchInProgress = true;
    startGameBtn.disabled = true;
    startGameBtn.textContent = '启动中...';

    tauri.invoke('game.launch', { profileName: currentVersionName }).then((result) => {
      if (result && result.ok) {
        // 异步启动成功，等待进度事件更新按钮
        startGameBtn.textContent = '正在启动游戏...';
      } else {
        startGameBtn.textContent = '启动失败';
        gameLaunchInProgress = false;
        alert((result && result.error) || '启动游戏失败');
        setTimeout(() => {
          startGameBtn.textContent = '开始游戏';
          startGameBtn.disabled = false;
        }, 2000);
      }
    }).catch((err) => {
      startGameBtn.textContent = '启动失败';
      gameLaunchInProgress = false;
      alert('启动游戏失败: ' + (err.message || '未知错误'));
      setTimeout(() => {
        startGameBtn.textContent = '开始游戏';
        startGameBtn.disabled = false;
      }, 2000);
    });
  });

  // ---- 自定义悬浮框 ----
  const tooltip = document.getElementById('customTooltip');

  function moveTooltip(e) {
    const x = e.clientX + 14;
    const y = e.clientY + 14;
    tooltip.style.left = x + 'px';
    tooltip.style.top = y + 'px';
  }

  function showTooltip(e) {
    const text = e.currentTarget.getAttribute('data-tip');
    if (!text) return;
    tooltip.textContent = text;
    moveTooltip(e);
    tooltip.classList.add('show');
  }

  function hideTooltip() {
    tooltip.classList.remove('show');
  }

  document.querySelectorAll('[data-tip]').forEach(el => {
    el.addEventListener('mouseenter', showTooltip);
    el.addEventListener('mousemove', moveTooltip);
    el.addEventListener('mouseleave', hideTooltip);
  });

  // ==================== 消息提示框系统 ====================
  const notificationContainer = document.getElementById('notificationContainer');
  let notificationId = 0;

  /**
   * 创建消息提示卡片
   * @param {Object} options - 配置选项
   * @param {string} options.title - 标题
   * @param {string} options.content - 内容
   * @param {string} options.type - 类型: 'info', 'success', 'warning', 'error'
   * @param {number} options.duration - 持续时间(毫秒)，默认5000
   * @param {string} options.icon - 图标URL(可选)
   * @param {Function} options.onClick - 点击回调(可选)
   * @returns {number} 通知ID
   */
  function showNotification(options) {
    const {
      title = '通知',
      content = '',
      type = 'info',
      duration = 5000,
      icon = null,
      onClick = null
    } = options;

    const id = ++notificationId;
    const card = document.createElement('div');
    card.className = 'notification-card';
    card.dataset.id = id;

    // 卡片内容
    let html = `
      <div class="notification-title">${title}</div>
      <div class="notification-content">${content}</div>
      <button class="notification-close" onclick="dismissNotification(${id})">&times;</button>
    `;

    if (icon) {
      html += `<img class="notification-icon" src="${icon}" alt="icon">`;
    }

    // 进度条
    html += `
      <div class="notification-progress">
        <div class="notification-progress-bar ${type}" style="transform: scaleX(1); transition-duration: ${duration}ms;"></div>
      </div>
    `;

    card.innerHTML = html;

    // 点击事件
    if (onClick) {
      card.style.cursor = 'pointer';
      card.addEventListener('click', (e) => {
        if (e.target.classList.contains('notification-close')) return;
        onClick();
        dismissNotification(id);
      });
    }

    // 添加到容器
    notificationContainer.appendChild(card);

    // 触发动画：从右侧滑入
    requestAnimationFrame(() => {
      requestAnimationFrame(() => {
        card.classList.add('show');
      });
    });

    // 启动进度条动画
    setTimeout(() => {
      const progressBar = card.querySelector('.notification-progress-bar');
      if (progressBar) {
        progressBar.style.transform = 'scaleX(0)';
      }
    }, 50);

    // 自动消失
    const timer = setTimeout(() => {
      dismissNotification(id);
    }, duration);

    // 存储定时器以便取消
    card.dataset.timer = timer;

    return id;
  }

  /**
   * 关闭消息提示卡片
   * @param {number} id - 通知ID
   */
  function dismissNotification(id) {
    const card = notificationContainer.querySelector(`[data-id="${id}"]`);
    if (!card) return;

    // 清除定时器
    const timer = parseInt(card.dataset.timer);
    if (timer) clearTimeout(timer);

    // 触发淡出动画
    card.classList.remove('show');
    card.classList.add('hide');

    // 动画结束后移除元素
    setTimeout(() => {
      card.remove();
    }, 400);
  }

  /**
   * 清除所有通知
   */
  function clearAllNotifications() {
    notificationContainer.innerHTML = '';
  }

  // 暴露到全局
  window.showNotification = showNotification;
  window.dismissNotification = dismissNotification;
  window.clearAllNotifications = clearAllNotifications;

})();