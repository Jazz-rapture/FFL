(() => {
  const loginIcon = '../resoures/icon/steve.png';
  const tauri = window.__tauricpp__;

  // 减弱动效偏好。CSS 侧有全局媒体查询兜底（把 animation/transition 时长压到 0.001ms），
  // 但 JS 用 setTimeout 编排的时序不受 CSS 约束，必须显式折叠，
  // 否则动画已经瞬时完成、界面还要干等两秒。
  const motionQuery = window.matchMedia('(prefers-reduced-motion: reduce)');
  const motionDelay = (ms) => (motionQuery.matches ? 0 : ms);

  // ---- 背景图：首帧先套上次的选择 ----
  // 真正的值以 config.json 为准（background.list 回来后校正），但那是异步的，
  // 等它回来再套会先亮一下默认图再跳到自定义图。这里留一份镜像，脚本执行时
  // 就同步把 CSS 变量改掉，首帧直接是对的图
  const BG_MIRROR_KEY = 'tauricpp.background';
  document.documentElement.style.setProperty(
    '--bg-image',
    `url("${readBackgroundMirror().url || 'https://tauricpp.app/resoures/background/pic1.svg'}")`
  );

  function readBackgroundMirror() {
    try {
      const raw = window.localStorage.getItem(BG_MIRROR_KEY);
      if (!raw) return {};
      const parsed = JSON.parse(raw);
      return parsed && typeof parsed === 'object' ? parsed : {};
    } catch (error) {
      return {};
    }
  }

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
  const loginTabs = document.getElementById('loginTabs');
  const loginInput = document.getElementById('loginInput');
  const loginInputGroup = document.getElementById('loginInputGroup');
  const loginMsa = document.getElementById('loginMsa');
  const loginMsaCode = document.getElementById('loginMsaCode');
  const loginMsaCodeValue = document.getElementById('loginMsaCodeValue');
  const loginMsaStatus = document.getElementById('loginMsaStatus');
  const loginMsaLink = document.getElementById('loginMsaLink');
  const loginThirdParty = document.getElementById('loginThirdParty');
  const loginTpUrl = document.getElementById('loginTpUrl');
  const loginTpName = document.getElementById('loginTpName');
  const loginTpPassword = document.getElementById('loginTpPassword');
  const loginTpStatus = document.getElementById('loginTpStatus');
  const loginTpProfiles = document.getElementById('loginTpProfiles');
  const loginBtn = document.getElementById('loginBtn');
  const loginBack = document.getElementById('loginBack');
  const transitionImg = document.getElementById('transitionImg');

  // ---- 图层切换（大厅层走单独的显示逻辑） ----
  function showLayer(layer, returnable) {
    [layerLogin, layerTransition].forEach(l => l.classList.remove('active'));
    layerLobby.classList.remove('active');
    if (layer) layer.classList.add('active');
    // “返回”只在从大厅再次进入登录界面时才给：首次启动时大厅还没出现过，
    // 没有可返回的对象
    if (layer === layerLogin) loginBack.hidden = !returnable;

    // 主界面元素动画控制
    const versionCard = document.querySelector('.version-card');
    const startGameBtn = document.querySelector('.start-game-btn');
    const accountCard = document.querySelector('.account-card');

    if (layer === layerLobby) {
      // 进入大厅：回到主页，主界面元素从下方滑入（每次都要重新滑入）
      // 先重置状态
      [downloadPage, settingsPage, resourcePage, onlinePage].forEach(p => p.classList.remove('active'));
      homePage.classList.add('active');
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
      // 页面区（副导航栏 + 内容面板）也要收起：
      // 它们是毛玻璃，必须各自淡出，不能靠父级 opacity（见页面初始化处说明）
      deactivatePages();
      slideOutSubSidebars();
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

  // ---- 登录方式切换 ----
  // 离线登录：自己输 ID；正版登录：微软设备代码流；
  // 三方登录：外置登录（authlib-injector / Yggdrasil），要填服务器地址 + 账号密码
  let loginMethod = 'offline';
  const loginTabButtons = Array.from(loginTabs.querySelectorAll('.login-tab'));

  function selectLoginMethod(method) {
    const flowActive = msaState === 'requesting' || msaState === 'waiting';
    // 重复点当前 tab 不该打断正在进行的授权
    if (method === loginMethod && flowActive) return;
    // 切到别的登录方式就把等待中的授权收掉，免得后台线程继续轮询
    if (flowActive) cancelMsaLogin(true);
    else msaState = 'idle';

    loginMethod = method;
    loginTabButtons.forEach(t => t.classList.toggle('active', t.dataset.method === method));
    // 三种方式各有各的输入区：离线输 ID，三方输服务器地址 + 账号密码，正版没有输入框
    loginInputGroup.classList.toggle('hidden', method !== 'offline');
    loginMsa.classList.toggle('hidden', method !== 'mojang');
    loginThirdParty.classList.toggle('hidden', method !== 'thirdparty');
    resetMsaPanel();
    resetThirdPartyPanel();
    loginBtn.textContent = '登录';
    loginBtn.disabled = false;
  }

  loginTabButtons.forEach((tab) => {
    tab.addEventListener('click', () => selectLoginMethod(tab.dataset.method));
  });

  // ---- 正版登录（微软设备代码流）----
  let msaState = 'idle';            // idle / requesting / waiting / done
  let msaVerificationUri = '';

  // 登录层的异步结果只在登录界面还开着时才有意义：用户可能中途点了“返回”
  // 退回大厅，这时迟到的结果不该再更新界面，更不该把人拽进过渡动画
  const loginLayerActive = () => layerLogin.classList.contains('active');

  const MSA_STAGE_TEXT = {
    xbox_live: '正在验证 Xbox Live 身份…',
    xsts: '正在获取 XSTS 授权…',
    minecraft: '正在获取 Minecraft 令牌…',
    entitlements: '正在检查该账号是否拥有 Minecraft…',
    profile: '正在获取角色信息…'
  };

  function resetMsaPanel() {
    loginMsaCode.hidden = true;
    loginMsaLink.hidden = true;
    loginMsaCodeValue.textContent = '';
    loginMsaStatus.classList.remove('error');
    loginMsaStatus.textContent = '点下面的按钮，用浏览器登录微软账号';
  }

  function setMsaStatus(text, isError) {
    loginMsaStatus.classList.toggle('error', !!isError);
    loginMsaStatus.textContent = text;
  }

  function failMsa(message) {
    msaState = 'idle';
    // 失败后这段代码已经作废，不要再摆在界面上误导用户
    loginMsaCode.hidden = true;
    loginMsaLink.hidden = true;
    setMsaStatus(message || '登录失败', true);
    loginBtn.textContent = '登录';
    loginBtn.disabled = false;
  }

  function cancelMsaLogin(silent) {
    tauri.invoke('auth.msa.cancel');
    msaState = 'idle';
    resetMsaPanel();
    if (!silent) setMsaStatus('已取消登录');
    loginBtn.textContent = '登录';
    loginBtn.disabled = false;
  }

  function startMsaLogin() {
    msaState = 'requesting';
    loginBtn.disabled = true;
    loginBtn.textContent = '获取代码中...';
    loginMsaCode.hidden = true;
    loginMsaLink.hidden = true;
    setMsaStatus('正在向微软申请设备代码…');

    tauri.invoke('auth.msa.start').then((result) => {
      if (!result || !result.ok) {
        failMsa((result && result.error) || '无法开始登录');
      }
    }).catch((err) => {
      failMsa((err && err.message) || '无法开始登录');
    });
  }

  // 拿到代码：展示出来并自动打开验证页
  tauri.listen('auth.msa.code', (event) => {
    if (!loginLayerActive()) return;
    msaState = 'waiting';
    msaVerificationUri = event.verificationUri || '';
    loginMsaCodeValue.textContent = event.userCode || '';
    loginMsaCode.hidden = false;
    loginMsaLink.hidden = !msaVerificationUri;
    setMsaStatus('正在等待你在浏览器中完成授权…');
    loginBtn.textContent = '取消登录';
    loginBtn.disabled = false;
    if (msaVerificationUri) tauri.invoke('shell.open', { url: msaVerificationUri });
  });

  tauri.listen('auth.msa.status', (event) => {
    const text = MSA_STAGE_TEXT[event.stage];
    if (text) setMsaStatus(text);
  });

  tauri.listen('auth.msa.result', (event) => {
    if (!loginLayerActive()) return;
    if (event.ok) {
      msaState = 'done';
      setMsaStatus(`登录成功：${event.name}`);
      loginMsaCode.hidden = true;
      loginMsaLink.hidden = true;
      loginBtn.textContent = '登录';
      loginBtn.disabled = false;
      startTransition();
    } else {
      failMsa(event.error);
    }
  });

  loginMsaLink.addEventListener('click', () => {
    if (msaVerificationUri) tauri.invoke('shell.open', { url: msaVerificationUri });
  });

  // ---- 三方登录（外置登录 / authlib-injector）----
  // 流程全在后端：先探测验证服务器拿到真正的 API 根地址，再走 Yggdrasil authenticate。
  // 前端只负责收输入、显示进度、多角色时让用户选一个；密码不回传、不留存
  let tpState = 'idle';   // idle / running / selecting / done

  function resetThirdPartyPanel() {
    loginTpStatus.hidden = true;
    loginTpStatus.classList.remove('error');
    loginTpStatus.textContent = '';
    loginTpProfiles.hidden = true;
    loginTpProfiles.innerHTML = '';
  }

  function setTpStatus(text, isError) {
    loginTpStatus.hidden = false;
    loginTpStatus.classList.toggle('error', !!isError);
    loginTpStatus.textContent = text;
  }

  function failThirdParty(message) {
    tpState = 'idle';
    setTpStatus(message || '登录失败', true);
    loginBtn.textContent = '登录';
    loginBtn.disabled = false;
  }

  function doThirdPartyLogin() {
    const url = loginTpUrl.value.trim();
    const username = loginTpName.value.trim();
    const password = loginTpPassword.value;

    if (!url) { setTpStatus('请输入验证服务器地址', true); return; }
    if (!username) { setTpStatus('请输入用户名', true); return; }
    if (!password) { setTpStatus('请输入密码', true); return; }

    tpState = 'running';
    loginBtn.disabled = true;
    loginBtn.textContent = '登录中...';
    loginTpProfiles.hidden = true;
    loginTpProfiles.innerHTML = '';
    setTpStatus('正在连接验证服务器...');

    tauri.invoke('auth.yggdrasil.login', { url, username, password })
      .then((result) => {
        if (!result || !result.ok) {
          failThirdParty((result && result.error) || '无法开始登录');
        }
      })
      .catch((err) => {
        failThirdParty((err && err.message) || '无法开始登录');
      })
      // 密码只在这一次请求里有意义，无论成败都立刻从界面上抹掉
      .then(() => { loginTpPassword.value = ''; });
  }

  tauri.listen('auth.yggdrasil.status', (event) => {
    if (event && event.stage) setTpStatus(event.stage);
  });

  tauri.listen('auth.yggdrasil.result', (event) => {
    if (!event) return;
    // 用户点了“返回”退回大厅后，这个迟到的结果不再处理
    if (!loginLayerActive()) return;

    // 验证服务器给了多个角色、且没有自动绑定：先让用户选一个
    if (event.ok && event.needProfileSelect) {
      tpState = 'selecting';
      setTpStatus('该账号下有多个角色，请选择要登录的角色');
      renderThirdPartyProfiles(event.profiles || []);
      loginBtn.textContent = '登录';
      loginBtn.disabled = false;
      return;
    }

    if (event.ok) {
      tpState = 'done';
      setTpStatus(`登录成功：${event.name}`);
      loginBtn.textContent = '登录';
      loginBtn.disabled = false;
      startTransition();
    } else {
      failThirdParty(event.error);
    }
  });

  function renderThirdPartyProfiles(profiles) {
    loginTpProfiles.innerHTML = '';
    loginTpProfiles.hidden = profiles.length === 0;

    profiles.forEach((profile) => {
      const item = document.createElement('button');
      item.type = 'button';
      item.className = 'login-tp-profile';
      item.textContent = profile.name || profile.id;
      item.addEventListener('click', () => {
        loginTpProfiles.hidden = true;
        tpState = 'running';
        loginBtn.disabled = true;
        loginBtn.textContent = '登录中...';
        setTpStatus('正在绑定角色...');
        tauri.invoke('auth.yggdrasil.select', { profileId: profile.id });
      });
      loginTpProfiles.appendChild(item);
    });
  }

  // 三个输入框都支持回车提交
  [loginTpUrl, loginTpName, loginTpPassword].forEach((input) => {
    input.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') doLogin();
    });
  });

  // ---- 登录逻辑 ----
  loginBtn.addEventListener('click', doLogin);
  loginInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') doLogin();
  });

  function doLogin() {
    if (loginMethod === 'mojang') {
      if (msaState === 'waiting') {
        cancelMsaLogin(false);
      } else if (msaState === 'idle' || msaState === 'done') {
        startMsaLogin();
      }
      return;
    }

    if (loginMethod === 'thirdparty') {
      doThirdPartyLogin();
      return;
    }

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
        // 记下这次登录的账号，启动游戏时按它取会话
        tauri.invoke('account.select', { index: index });
        return tauri.invoke('account.check', { index: index });
      }
      return null;
    }).then((checkResult) => {
      if (checkResult && checkResult.has_account) {
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

    // 减弱动效下直接进大厅：过渡层是三段式编排，压成 0 反而会闪一下
    if (motionQuery.matches) {
      showLayer(layerLobby);
      loadAccountCard();
      return;
    }

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
  const onlinePage = document.getElementById('onlinePage');
  const sidebarIcons = document.querySelectorAll('.sidebar-icon');

  const allPages = [homePage, downloadPage, settingsPage, resourcePage, onlinePage];
  // 页面退场 = 摘掉 .active。内容面板/副导航栏的淡出与滑出都由它们自身的
  // transition 驱动，所以摘得越早，退场开始得越早（见 switchPage）。
  const deactivatePages = () => allPages.forEach(p => p.classList.remove('active'));

  // 初始化：主页激活，其它页面隐藏
  // 注意：不要在页面上写 style.opacity —— 页面是内容面板（毛玻璃）的祖先，
  // 祖先透明度小于 1 会让 backdrop-filter 失去背景采样，切换动画期间模糊会消失。
  // 显隐/命中测试全部由 .active 类驱动（见 .home-page/.download-page 等规则）。
  homePage.classList.add('active');

  function switchPage(pageName) {
    if (pageName === 'home' && homePage.classList.contains('active')) return;
    if (pageName === 'download' && downloadPage.classList.contains('active')) return;
    if (pageName === 'settings' && settingsPage.classList.contains('active')) return;
    if (pageName === 'resource' && resourcePage.classList.contains('active')) return;
    if (pageName === 'online' && onlinePage.classList.contains('active')) return;

    // 离开主页时，让版本卡片和角色卡片滑出
    if (homePage.classList.contains('active')) {
      slideOutHomeCards();
    }

    // 退场必须从点击这一刻就摘掉 .active：内容面板是毛玻璃，只能靠自身 opacity 淡出，
    // 若拖到下面 200ms 的回调里才摘，主展示区会先原样静止 200ms 再消失
    //（看起来"切出时停留过久"）。
    deactivatePages();

    // 离开有副导航栏的页面时，让副导航栏向左滑出；内容面板靠自身的 opacity 淡出
    slideOutSubSidebars();

    // 立即开始加载新页面内容（不等待淡出完成）
    if (pageName === 'download') {
      // 从别处跳进来时可能已经指定了要看的子分类（例如下载任务页），
      // 没指定才回落到默认的「原版游戏」
      activeCategory = pendingDownloadCategory || 'game';
      pendingDownloadCategory = null;
      subSidebarItems.forEach(i => {
        i.classList.toggle('active', i.dataset.category === activeCategory);
      });
      loadDownloadContent(activeCategory);
    } else if (pageName === 'settings') {
      loadSettingsContent(activeSettingsCategory);
    } else if (pageName === 'resource') {
      loadResourceContent();
    } else if (pageName === 'online') {
      loadOnlineContent(activeOnlineCategory);
    }

    // 退场进行到一半时再让新页面进场
    setTimeout(() => {
      // 这里必须再摘一次：连续快速切换时，前一次的回调可能已经加回了旧页面
      deactivatePages();

      if (pageName === 'home') {
        homePage.classList.add('active');
        // 切回主页时，让版本卡片和角色卡片重新滑入
        slideInHomeCards();
      } else if (pageName === 'download') {
        downloadPage.classList.add('active');
        // 副导航栏滑入
        slideInSubSidebar(downloadPage);
      } else if (pageName === 'settings') {
        settingsPage.classList.add('active');
        // 副导航栏滑入
        slideInSubSidebar(settingsPage);
      } else if (pageName === 'resource') {
        resourcePage.classList.add('active');
        // 副导航栏滑入
        slideInSubSidebar(resourcePage);
      } else if (pageName === 'online') {
        onlinePage.classList.add('active');
        // 副导航栏滑入
        slideInSubSidebar(onlinePage);
      }
      // 页面真正切好之后同步一次：进出「下载任务」页会决定进度类小通知显不显示
      setTasksViewActive(pageName === 'download' && activeCategory === 'tasks');
    }, motionDelay(200)); // 退场动画走一半再让新页面进场
  }

  // 由代码切页时用这个：侧边栏图标的高亮只在图标自身被点击时才自动维护，
  // 从主页卡片跳过去的话得手动同步，否则高亮还停在「主页」上
  function switchPageWithSidebar(pageName) {
    sidebarIcons.forEach(i => i.classList.toggle('active', i.dataset.tab === pageName));
    switchPage(pageName);
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
  // 必须同时收起其它页面的副导航栏：快速连续切换时，上一次 200ms 回调可能刚把
  // 旧页面的副导航栏设成 slide-in，而 slideOutSubSidebars() 只处理"当时已经
  // slide-in"的那些，不在这里兜底就会出现两条副导航栏同时可见。
  function slideInSubSidebar(page) {
    const subSidebar = page.querySelector('.sub-sidebar');
    if (!subSidebar) return;

    document.querySelectorAll('.sub-sidebar').forEach(other => {
      if (other !== subSidebar) {
        other.classList.remove('slide-in');
        other.classList.add('slide-out');
      }
    });

    subSidebar.classList.remove('slide-in', 'slide-out');
    void subSidebar.offsetWidth; // 强制重排
    subSidebar.classList.add('slide-in');
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
  // 代码主动跳到「下载」页时用它指定要落到哪个子分类，
  // switchPage 读到后清空，这样用户手动点侧边栏时仍然是默认分类
  let pendingDownloadCategory = null;

  // 下载内容区的视图代际：每次切换分类/详情页都会+1。
  // 命令在后台线程执行（不再阻塞UI），响应回来时用户可能已经切走了，
  // 异步回调必须先校验代际，否则旧响应会把当前视图覆盖掉。
  let downloadViewSeq = 0;

  // 下载页的三个社区资源分类共用同一套浏览器：都走 Modrinth 搜索，只是
  // project_type 不同（资源包、光影包对加载器没有要求，所以不带加载器筛选）。
  // 另存为的默认目录由后端按当前版本的隔离设置算好，这里不用关心路径规则。
  const RESOURCE_KINDS = {
    mods: { type: 'mod', label: '模组', placeholder: '搜索模组名称...', hasLoader: true },
    resourcepack: { type: 'resourcepack', label: '资源包', placeholder: '搜索资源包名称...', hasLoader: false },
    shaderpack: { type: 'shader', label: '光影包', placeholder: '搜索光影包名称...', hasLoader: false }
  };

  // 当前正在浏览的资源类型，另存为时要用它决定默认目录
  let currentResourceKind = RESOURCE_KINDS.mods;

  // 当前实例用的原版游戏版本号（例如 1.20.1）：文件列表里用它标记并展开「所选版本」
  let currentInstanceVersion = '';

  function loadDownloadContent(category) {
    if (category === 'game') {
      loadVersionList();
    } else if (category === 'tasks') {
      renderDownloadTasks(true);
    } else if (RESOURCE_KINDS[category]) {
      loadResourceBrowser(category);
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

  // ==================== Mod 译名表 ====================
  // 本地译名映射表 data/mod-names.txt：构建时随前端一起打进 exe，WebView 直接取内存里的这份，
  // 不走网络也不走 Bridge。读取失败就整表为空，页面照常只显示英文名。
  //
  // 解析同时兼容两种写法，所以用户可以直接把 PCL 的 WikiEntries.txt 内容贴进来：
  //   1) 本仓库的简表             <Modrinth slug>|<中文名>
  //   2) PCL 的 WikiEntries.txt   一行里可以用 ¨ 分隔多个条目，每条 <slug 描述>|<中文名>；
  //      描述里 @slug = 只在 Modrinth、slug@ = 两端同名、cf@mr = 两端各自的 slug，
  //      译名里的 * 展开成「 (英文名)」。文件末尾那行浏览量数据没有 |，会被自然跳过。
  let modNameTable = null;
  let modNameTablePromise = null;

  // 整份文件用的是哪种写法：出现 ¨、或者 slug 段里出现 @，就是 PCL 的 WikiEntries.txt。
  // 两种写法的区别只在「没有 @ 的条目标在哪一端」：简表里它就是 Modrinth slug，
  // 而在 PCL 的格式里它只存在于 CurseForge，对按 Modrinth 查表的我们没有意义。
  function isWikiEntriesFormat(text) {
    return text.split('\n').some((rawLine) => {
      const line = rawLine.replace(/\r$/, '').trim();
      if (!line || line.charAt(0) === '#') return false;
      return line.includes('\u00A8') || line.split('|')[0].includes('@');
    });
  }

  function parseModNameTable(text) {
    const table = new Map();
    const wikiFormat = isWikiEntriesFormat(text);
    const slugToEnglish = (slug) => slug.split('-').filter(Boolean)
      .map(w => w.charAt(0).toUpperCase() + w.slice(1)).join(' ');
    text.split('\n').forEach((rawLine) => {
      const line = rawLine.replace(/\r$/, '').trim();
      if (!line || line.charAt(0) === '#') return;
      line.split('\u00A8').forEach((entry) => {
        const parts = entry.split('|');
        if (parts.length < 2) return;
        const spec = parts[0].trim();
        let name = parts.slice(1).join('|').trim();
        if (!spec || !name) return;

        // 按 PCL 的规则把 slug 段拆到两端
        let curseforge = '';
        let modrinth = '';
        if (spec.charAt(0) === '@') {
          modrinth = spec.slice(1);
        } else if (spec.charAt(spec.length - 1) === '@') {
          curseforge = spec.slice(0, -1);
          modrinth = curseforge;
        } else if (spec.includes('@')) {
          const both = spec.split('@');
          curseforge = both[0];
          modrinth = both[1] || '';
        } else if (wikiFormat) {
          curseforge = spec;
        } else {
          modrinth = spec;
        }
        modrinth = modrinth.trim().toLowerCase();
        if (!modrinth) return;

        // * 展开用的英文名取 PCL 里的第一个 slug（有 CurseForge 侧就用它）
        if (name.includes('*')) {
          name = name.split('*').join(' (' + slugToEnglish((curseforge || modrinth).trim()) + ')');
        }
        if (!table.has(modrinth)) table.set(modrinth, name);
      });
    });
    return table;
  }

  function loadModNameTable() {
    if (!modNameTablePromise) {
      modNameTablePromise = fetch('data/mod-names.txt')
        .then((resp) => (resp && resp.ok ? resp.text() : ''))
        .then((text) => parseModNameTable(text))
        .catch(() => new Map())
        .then((table) => { modNameTable = table; return table; });
    }
    return modNameTablePromise;
  }

  // 查表按 Modrinth slug 精确匹配（忽略大小写）
  function modChineseName(mod) {
    if (!modNameTable) return '';
    const slug = String((mod && mod.slug) || '').trim().toLowerCase();
    return (slug && modNameTable.get(slug)) || '';
  }

  // 表里有译名就「中文名 + 原英文名」；译名本身已经含英文名时不重复显示
  function modDisplayNameHtml(mod) {
    const english = String((mod && (mod.title || mod.slug)) || '').trim();
    const chinese = modChineseName(mod);
    if (!chinese) return english;
    if (chinese.toLowerCase().includes(english.toLowerCase())) return chinese;
    return chinese + '<span class="mod-name-en">' + english + '</span>';
  }

  loadModNameTable();

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

  // 每次搜索领一个令牌。切走分类或再次搜索时令牌变化，旧的轮询和结果一律作废
  let modSearchToken = 0;
  // 文件列表（点开某个项目后的版本列表）单独的令牌，返回列表时作废还挂着的请求
  let fileListToken = 0;

  // 异步命令的通用轮询：后端把长任务（modrinth.* / java.scan）丢到独立线程执行，
  // 不占 Bridge 那个单线程的命令队列，结果要自己拉。
  // alive() 为假就立刻停止轮询 —— 已经切走的视图没必要继续发无用请求。
  function pollAsyncResult(cmd, args, alive, timeoutMs = 60000) {
    return new Promise((resolve, reject) => {
      const deadline = Date.now() + timeoutMs;
      const tick = () => {
        if (alive && !alive()) { reject(new Error('stale')); return; }
        if (Date.now() > deadline) { reject(new Error('timeout')); return; }
        tauri.invoke(cmd, args).then((resp) => {
          if (alive && !alive()) { reject(new Error('stale')); return; }
          if (resp && resp.hasResult) resolve(resp.data);
          else setTimeout(tick, 200);
        }).catch(() => setTimeout(tick, 400));
      };
      tick();
    });
  }

  // 后端按 kinds 分别留存结果：标签靠 projectType 区分，其余靠请求号/任务号。
  // 下载可能几十 MB，超时时间单独给长一些。
  function pollModrinth(kind, id, alive, options) {
    const opts = options || {};
    return pollAsyncResult(
      'modrinth.poll',
      Object.assign({ kind, requestId: id, taskId: id }, opts.extra || {}),
      alive,
      opts.timeoutMs || 60000
    );
  }

  function loadResourceBrowser(category) {
    const kind = RESOURCE_KINDS[category];
    currentResourceKind = kind;
    const view = ++downloadViewSeq;
    fileListToken++;
    modSearchState = { query: '', versions: [], loaders: [], categories: [], index: 'relevance', offset: 0, limit: 20, hits: [], totalHits: 0 };

    downloadContent.innerHTML = `
      <div class="mod-browser">
        <div class="mod-search-bar">
          <input type="text" class="mod-search-input" id="modSearchInput" placeholder="${kind.placeholder}">
          <select class="mod-filter-select" id="modVersionFilter"><option value="">游戏版本</option></select>
          ${kind.hasLoader ? '<select class="mod-filter-select" id="modLoaderFilter"><option value="">加载器</option></select>' : ''}
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
    // 资源包和光影包不区分加载器，这一栏根本没渲染出来
    const loaderFilter = document.getElementById('modLoaderFilter');
    const categoryFilter = document.getElementById('modCategoryFilter');
    const searchBtn = document.getElementById('modSearchBtn');
    const resetBtn = document.getElementById('modResetBtn');
    const resultsDiv = document.getElementById('modResults');

    const aliveTags = () => view === downloadViewSeq;

    // 当前实例的游戏版本：只读本地配置，拿到后用来在文件列表里标记「所选版本」
    tauri.invoke('game.getResourcePaths').then((paths) => {
      if (!aliveTags() || !paths || !paths.ok) return;
      currentInstanceVersion = paths.gameVersion || '';
    }).catch(() => {});

    const applyTags = (tags) => {
      if (!aliveTags()) return;
      if (tags && Array.isArray(tags.versions)) {
        tags.versions.forEach(v => {
          const o = document.createElement('option');
          o.value = v; o.textContent = v;
          versionFilter.appendChild(o);
        });
      }
      if (tags && Array.isArray(tags.loaders) && loaderFilter) {
        tags.loaders.forEach(l => {
          const o = document.createElement('option');
          o.value = l.value; o.textContent = l.label.charAt(0).toUpperCase() + l.label.slice(1);
          loaderFilter.appendChild(o);
        });
      }
      if (tags && Array.isArray(tags.categories)) {
        tags.categories.forEach(c => {
          const o = document.createElement('option');
          o.value = c.value; o.textContent = c.label;
          categoryFilter.appendChild(o);
        });
      }
      doModSearch();
    };

    const failTags = () => {
      if (!aliveTags()) return;
      resultsDiv.innerHTML = '<div class="mod-empty"><p>加载筛选选项失败</p></div>';
    };

    // 筛选选项走网络，后端在独立线程里抓；每种资源类型各缓存一份，
    // 再进同一个分类就是同步返回，不用再等
    tauri.invoke('modrinth.getTags', { projectType: kind.type }).then((result) => {
      if (!aliveTags()) return;
      if (result && result.tags) { applyTags(result.tags); return; }
      if (result && result.async) {
        pollModrinth('tags', 0, aliveTags, { extra: { projectType: kind.type } })
          .then(applyTags).catch(failTags);
        return;
      }
      failTags();
    }).catch(() => failTags());

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
      if (loaderFilter) loaderFilter.value = '';
      categoryFilter.value = '';
      modSearchState = { query: '', versions: [], loaders: [], categories: [], index: 'relevance', offset: 0, limit: 20, hits: [], totalHits: 0 };
      doModSearch();
    });

    versionFilter.addEventListener('change', () => {
      modSearchState.offset = 0; modSearchState.hits = []; doModSearch();
    });
    if (loaderFilter) {
      loaderFilter.addEventListener('change', () => {
        modSearchState.offset = 0; modSearchState.hits = []; doModSearch();
      });
    }
    categoryFilter.addEventListener('change', () => {
      modSearchState.offset = 0; modSearchState.hits = []; doModSearch();
    });
  }

  function doModSearch() {
    const view = downloadViewSeq;
    const token = ++modSearchToken;
    const resultsDiv = document.getElementById('modResults');
    if (!resultsDiv) return;

    const versionFilter = document.getElementById('modVersionFilter');
    const loaderFilter = document.getElementById('modLoaderFilter');
    const categoryFilter = document.getElementById('modCategoryFilter');

    // 切走分类（视图代际变化）或又发起了一次新搜索（令牌变化）都算这张视图已作废，
    // 作废后不再渲染、也不再继续轮询
    const alive = () => token === modSearchToken && view === downloadViewSeq;

    if (modSearchState.offset === 0) {
      resultsDiv.innerHTML = '<div class="mod-loading"><div class="download-spinner"></div><p>搜索中...</p></div>';
    }

    // 搜索同样在独立线程里跑，这里先拿到请求号，再轮询结果
    tauri.invoke('modrinth.search', {
      query: modSearchState.query,
      projectType: currentResourceKind.type,
      index: modSearchState.index,
      offset: modSearchState.offset,
      limit: modSearchState.limit,
      versions: versionFilter ? versionFilter.value : '',
      loaders: loaderFilter ? loaderFilter.value : '',
      categories: categoryFilter ? categoryFilter.value : ''
    }).then((started) => {
      if (!alive()) return null;
      if (!started || !started.requestId) throw new Error('search not accepted');
      return pollModrinth('search', started.requestId, alive);
    }).then((data) => {
      if (!alive() || !data) return;
      if (!data.ok) throw new Error(data.error || 'search failed');

      const hits = data.hits || [];
      modSearchState.totalHits = data.total_hits || 0;

      if (modSearchState.offset === 0) {
        modSearchState.hits = hits;
      } else {
        modSearchState.hits = modSearchState.hits.concat(hits);
      }

      renderModResults();
    }).catch(() => {
      if (!alive() || modSearchState.offset !== 0) return;
      resultsDiv.innerHTML = '<div class="mod-empty"><p>搜索失败，请稍后重试</p></div>';
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

    // 译名表还没到就先按英文名渲染，到了再重画一次（正常情况启动时就已经加载好了）
    if (!modNameTable) {
      loadModNameTable().then(() => {
        if (modSearchState.hits.length) renderModResults();
      });
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
            <span class="mod-card-name">${modDisplayNameHtml(mod)}</span>
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
        <button class="mod-card-web" title="在浏览器中打开项目页">打开网页</button>
      `;

      // 主体点击进入文件列表（选版本 → 另存为），打开项目页挪到右侧小按钮上
      card.addEventListener('click', () => openResourceFiles(mod));
      card.querySelector('.mod-card-web').addEventListener('click', (e) => {
        e.stopPropagation();
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

  // 把文件大小格式化成人类可读的
  function formatFileSize(bytes) {
    if (!bytes || bytes <= 0) return '未知大小';
    if (bytes >= 1048576) return (bytes / 1048576).toFixed(1) + ' MB';
    if (bytes >= 1024) return (bytes / 1024).toFixed(0) + ' KB';
    return bytes + ' B';
  }

  // 点开一个项目：把结果区换成它的文件列表（选完版本再另存为）。
  // 搜索栏里的游戏版本/加载器筛选会一起带过去，只列对得上的文件
  function openResourceFiles(mod) {
    const resultsDiv = document.getElementById('modResults');
    if (!resultsDiv) return;

    const view = downloadViewSeq;
    const token = ++fileListToken;

    const versionFilter = document.getElementById('modVersionFilter');
    const loaderFilter = document.getElementById('modLoaderFilter');
    const gameVersion = versionFilter ? versionFilter.value : '';
    const loader = loaderFilter ? loaderFilter.value : '';

    resultsDiv.innerHTML = `
      <div class="mod-files">
        <div class="mod-files-head">
          <button class="mod-files-back" id="modFilesBack">← 返回</button>
          <span class="mod-files-title">${modDisplayNameHtml(mod)}</span>
        </div>
        <div class="mod-files-body" id="modFilesBody">
          <div class="mod-loading"><div class="download-spinner"></div><p>正在获取文件列表...</p></div>
        </div>
      </div>
    `;

    // 返回时作废这次的文件列表请求，避免它回来后又把列表顶掉
    document.getElementById('modFilesBack').addEventListener('click', () => {
      fileListToken++;
      renderModResults();
    });

    const alive = () => token === fileListToken && view === downloadViewSeq;
    const body = document.getElementById('modFilesBody');

    tauri.invoke('modrinth.versions', {
      projectId: mod.project_id || '',
      slug: mod.slug || '',
      gameVersion,
      loader
    }).then((started) => {
      if (!alive()) return null;
      if (!started || !started.requestId) throw new Error('files not accepted');
      return pollModrinth('versions', started.requestId, alive);
    }).then((data) => {
      if (!alive() || !data) return;
      if (!data.ok) throw new Error(data.error || 'failed');
      renderResourceFiles(body, data.files || [], mod, [gameVersion, loader].filter(Boolean).join(' / '));
    }).catch(() => {
      if (!alive()) return;
      body.innerHTML = '<div class="mod-empty"><p>获取文件列表失败，请稍后重试</p></div>';
    });
  }

  // 把文件名里的游戏版本归一成索引键：正式版按自身版本号分组，
  // 快照/预览归到"快照版"，远古版本（a/b 开头）单独一类，
  // 认不出来的归"其他"（与 PCL 的分组口径一致）
  function gameVersionGroup(name) {
    if (!name) return '其他';
    if (/^\d+\.\d+(\.\d+)?$/.test(name)) return name;          // 1.21 / 1.21.4
    if (/^\d{2}w\d{2}[a-z]$/i.test(name)) return '快照版';       // 24w14a / 14w02a
    if (/(pre|rc|snapshot)/i.test(name)) return '快照版';        // 1.21.4-pre1 / 1.21-rc1
    if (/^[ab]\d/i.test(name) || /(alpha|beta)/i.test(name)) return '远古版';  // a1.2.6 / b1.7.3
    return '其他';
  }

  // 版本号从新到旧（1.21.4 > 1.21 > 1.20.1）
  function compareVersionsDesc(a, b) {
    const pa = String(a).split('.').map(n => parseInt(n, 10) || 0);
    const pb = String(b).split('.').map(n => parseInt(n, 10) || 0);
    for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
      const diff = (pb[i] || 0) - (pa[i] || 0);
      if (diff !== 0) return diff;
    }
    return 0;
  }

  // 某一组下所有文件涉及的加载器（去掉 minecraft 这个每个文件都带的占位）
  function groupLoaders(groupFiles) {
    const all = new Set();
    groupFiles.forEach(f => (f.loaders || []).forEach(l => {
      if (l !== 'minecraft') all.add(l);
    }));
    return Array.from(all).join(' / ');
  }

  // 单个文件行：文件名 + 徽标 + 版本号 + 支持的版本范围 + 大小 + 日期 + 另存为
  function buildFileRow(file, groupKey, mod) {
    const TYPE_LABELS = { release: '正式版', beta: '测试版', alpha: '内测版' };
    const info = file.file || {};

    const row = document.createElement('div');
    row.className = 'mod-file-row';

    const versions = file.gameVersions || [];
    const versionRange = versions.length > 3
      ? versions[versions.length - 1] + ' ~ ' + versions[0]
      : (versions.join(', ') || '未知版本');
    const loaders = (file.loaders || []).filter(l => l !== 'minecraft').join(' / ');
    const date = file.datePublished ? new Date(file.datePublished).toLocaleDateString('zh-CN') : '';

    row.innerHTML = `
      <div class="mod-file-info">
        <div class="mod-file-name">${info.filename || file.name || ''}</div>
        <div class="mod-file-meta">
          <span class="mod-file-badge mod-file-badge-${file.versionType}">${TYPE_LABELS[file.versionType] || file.versionType || ''}</span>
          <span>${file.versionNumber || ''}</span>
          ${loaders ? '<span>' + loaders + '</span>' : ''}
          ${groupKey === currentInstanceVersion ? '' : '<span>支持 ' + versionRange + '</span>'}
          <span>${formatFileSize(info.size)}</span>
          <span>${date}</span>
        </div>
      </div>
      <button class="mod-file-save">另存为</button>
    `;

    row.querySelector('.mod-file-save').addEventListener('click', (e) => {
      e.stopPropagation();
      saveResourceFile(file, mod, e.currentTarget);
    });
    return row;
  }

  // 按游戏版本索引，和 PCL 一样：外层只列游戏版本，点开才展开该版本下的文件。
  // 一个文件往往支持多个游戏版本，所以它会出现在每个对应版本下面
  function renderResourceFiles(body, files, mod, filterText) {
    if (files.length === 0) {
      body.innerHTML = filterText
        ? `<div class="mod-empty"><p>没有适配当前筛选（${filterText}）的文件</p><p class="mod-empty-hint">把上方的游戏版本 / 加载器改成“全部”再看看</p></div>`
        : '<div class="mod-empty"><p>该项目还没有发布文件</p></div>';
      return;
    }

    const groups = new Map();
    files.forEach(file => {
      const seen = new Set();
      (file.gameVersions || []).forEach(gv => {
        const key = gameVersionGroup(gv);
        if (seen.has(key)) return;
        seen.add(key);
        if (!groups.has(key)) groups.set(key, []);
        groups.get(key).push(file);
      });
    });

    if (groups.size === 0) {
      body.innerHTML = '<div class="mod-empty"><p>该项目的文件没有标明支持的游戏版本</p></div>';
      return;
    }

    // 正式版按版本号从新到旧，快照版、远古版和"其他"排在后面
    const rank = key => (key === '快照版' ? 1 : key === '远古版' ? 2 : key === '其他' ? 3 : 0);
    const keys = Array.from(groups.keys()).sort((a, b) => {
      if (rank(a) !== rank(b)) return rank(a) - rank(b);
      return compareVersionsDesc(a, b);
    });
    // 当前实例用的版本提到最前面，一眼就能看到该装哪个
    const preferred = keys.indexOf(currentInstanceVersion);
    if (preferred > 0) {
      keys.splice(preferred, 1);
      keys.unshift(currentInstanceVersion);
    }

    const list = document.createElement('div');
    list.className = 'mod-ver-list';

    keys.forEach(key => {
      const groupFiles = groups.get(key);
      const isCurrent = key === currentInstanceVersion;
      // 只有一个版本可选、或者正好是当前实例的版本，默认展开（与 PCL 一致）
      const expanded = keys.length === 1 || isCurrent;

      const group = document.createElement('div');
      group.className = 'mod-ver-group';

      const head = document.createElement('button');
      head.className = 'mod-ver-head';
      head.innerHTML = `
        <span class="mod-ver-caret${expanded ? ' is-open' : ''}">▶</span>
        <span class="mod-ver-name">${key}</span>
        ${isCurrent ? '<span class="mod-ver-current">所选版本</span>' : ''}
        <span class="mod-ver-count">${groupFiles.length} 个文件</span>
        <span class="mod-ver-loaders">${groupLoaders(groupFiles)}</span>
      `;

      const filesWrap = document.createElement('div');
      filesWrap.className = 'mod-ver-files';
      filesWrap.hidden = !expanded;
      groupFiles.forEach(file => filesWrap.appendChild(buildFileRow(file, key, mod)));

      head.addEventListener('click', () => {
        filesWrap.hidden = !filesWrap.hidden;
        head.querySelector('.mod-ver-caret').classList.toggle('is-open', !filesWrap.hidden);
        if (!filesWrap.hidden) replayReveal(filesWrap);
      });

      group.appendChild(head);
      group.appendChild(filesWrap);
      list.appendChild(group);
    });

    body.innerHTML = '';
    body.appendChild(list);
  }

  // 另存为 → 下载。默认目录由后端按版本隔离算好（实例内的 mods / resourcepacks / shaderpacks），
  // 用户在对话框里可以改到任何地方
  async function saveResourceFile(file, mod, saveBtn) {
    const info = file.file || {};
    if (!info.url) {
      showNotification({ title: '无法下载', content: '该文件没有可用的下载地址', type: 'error', duration: 4000 });
      return;
    }
    const fileName = info.filename || ((mod && (mod.title || mod.slug)) || currentResourceKind.label);

    let chosen;
    try {
      // 把文件的游戏版本和加载器一起交给后端：它先判断适不适配当前实例，
      // 适配就先问一句要不要直接放进游戏文件夹（放进去重启游戏就会被加载）
      chosen = await tauri.invoke('resource.saveAs', {
        projectType: currentResourceKind.type,
        fileName,
        gameVersions: file.gameVersions || [],
        loaders: file.loaders || []
      });
    } catch (e) {
      showNotification({ title: '另存为失败', content: '无法打开保存对话框', type: 'error', duration: 4000 });
      return;
    }
    // 用户取消了对话框，什么都不做
    if (!chosen || !chosen.ok || chosen.canceled || !chosen.path) return;

    const originalText = saveBtn.textContent;
    saveBtn.disabled = true;
    saveBtn.textContent = '下载中...';

    // 资源下载后端不给逐字节进度（单文件、走 Modrinth CDN），
    // 任务卡上就只显示"进行中"，速度/剩余时间留空
    const taskId = createDownloadTask({
      kind: 'resource',
      title: fileName,
      subtitle: '正在下载…'
    });
    openTasksPage();

    try {
      const started = await tauri.invoke('resource.download', {
        url: info.url,
        path: chosen.path,
        sha1: info.sha1 || ''
      });
      if (!started || !started.taskId) throw new Error('下载未被受理');

      // 模组几十 MB、光影包可能上百 MB，给足超时
      const result = await pollModrinth('download', started.taskId, null, { timeoutMs: 600000 });
      if (!result || !result.ok) throw new Error((result && result.error) || '下载失败');

      finishDownloadTask(taskId, 'done', '');

      saveBtn.textContent = '已保存';
      showNotification({
        title: chosen.direct ? '已加入游戏文件夹' : '下载完成',
        content: chosen.direct
          ? `${fileName} → ${chosen.path}（重启游戏后加载）`
          : `${fileName} → ${chosen.path}`,
        type: 'success',
        duration: 6000,
        sticky: true,
        onClick: openTasksPage
      });
      setTimeout(() => {
        saveBtn.disabled = false;
        saveBtn.textContent = originalText;
      }, 3000);
    } catch (err) {
      saveBtn.disabled = false;
      saveBtn.textContent = originalText;
      finishDownloadTask(taskId, 'failed', err.message || '下载失败');
      showNotification({
        title: '下载失败',
        content: err.message || '下载失败',
        type: 'error',
        duration: 8000,
        sticky: true
      });
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
          if (open) replayReveal(body);
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
    const phaseLabelMap = { version: '版本信息', assets: '资源文件', libraries: '库文件', client: '客户端' };

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
      <select class="modloader-type-select">
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
      <select class="modloader-version-select">
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

    // 下载进度同时投一份到右上角消息弹窗：常驻卡片，随进度更新，
    // 结束后就地变成结果提示（成功 / 部分失败 / 失败）再自动消失。
    // 页面里那个进度条只在下载页可见，切走就看不到了，弹窗不挑页面。
    let downloadNoticeId = null;
    let downloadTaskId = null;
    // 下载信息（通知卡 / 任务页）的统一刷新间隔
    const DOWNLOAD_UI_INTERVAL_MS = 500;
    let lastDownloadUiAt = 0;

    const openDownloadNotice = (versionName) => {
      downloadTaskId = createDownloadTask({
        kind: 'vanilla',
        title: '游戏本体 ' + versionName,
        subtitle: '正在获取版本信息…'
      });
      // 任务一创建就把用户带到下载任务页，进度、速度、剩余时间都在那儿看
      openTasksPage();

      downloadNoticeId = showNotification({
        title: '下载 ' + versionName,
        content: '准备中...',
        type: 'info',
        sticky: true,
        tag: 'download',
        onClick: openTasksPage
      });
    };

    const settleDownloadNotice = (type, title, content) => {
      // 任务的最终状态跟着通知走：只有 success 才算完成，
      // warning（部分文件失败）按失败记，用户才知道得回来看
      if (downloadTaskId !== null) {
        if (type === 'success') {
          finishDownloadTask(downloadTaskId, 'done', '');
        } else if (type === 'warning') {
          finishDownloadTask(downloadTaskId, 'failed', '部分文件下载失败');
        } else {
          finishDownloadTask(downloadTaskId, 'failed', content || '下载失败');
        }
        downloadTaskId = null;
      }
      if (downloadNoticeId === null) return;
      settleNotification(downloadNoticeId, { title, content, type });
      downloadNoticeId = null;
    };

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
      // 优先显示字节数。只有文件计数的话，"客户端 0/1" 在整个 22MB 的
      // 下载过程中都不动，看着像卡住；字节数才能给出真实进展
      let text = label;
      if (totalBytes > 0) {
        text += ' ' + formatSize(downloadedBytes) + ' / ' + formatSize(totalBytes);
        if (total > 1) text += '（' + done + '/' + total + ' 个文件）';
      } else if (total > 0) {
        text += ' ' + done + '/' + total;
      }
      if (failed > 0) text += ' (失败 ' + failed + ')';
      progressText.textContent = text;

      // 通知卡和任务页都按 500ms 节流：进度事件在多线程下非常密集，
      // 每条都改一次 DOM 只会让数字乱跳
      const now = Date.now();
      if (now - lastDownloadUiAt >= DOWNLOAD_UI_INTERVAL_MS) {
        lastDownloadUiAt = now;
        if (downloadNoticeId !== null) {
          updateNotification(downloadNoticeId, { content: text + ' · ' + percent + '%', percent });
        }
      }

      // 模型每次都更新，界面的刷新节奏交给定时器
      updateDownloadTask(downloadTaskId, {
        subtitle: text,
        phase,
        percent,
        completedFiles: done,
        totalFiles: total,
        totalBytes,
        downloadedBytes
      });
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
              setTimeout(poll, 500);
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
      openDownloadNotice(versionName);

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
            // 客户端 jar 有 20+ MB，后端要靠这个体积判断走不走分段下载
            size: versionResult.clientDownload.size || 0,
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
          if (downloadNoticeId !== null) {
            updateNotification(downloadNoticeId, { content: '安装模组加载器中...', percent: 100 });
          }

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
          settleDownloadNotice('error', '下载失败', loaderResult.error || '模组加载器安装失败');
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
          settleDownloadNotice('warning', '下载完成（部分失败）',
                              allFailedFiles.length + ' 个文件下载失败，可在下载页重试');
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
          settleDownloadNotice('success', '下载完成', versionName + ' 已安装');
          tauri.invoke('version.select', { profileName: nameInput.value.trim() || v.id })
            .then(() => loadVersionCard());
        }
      }).catch((err) => {
        startBtn.disabled = false;
        startBtn.textContent = '开始下载';
        progressArea.style.display = 'none';
        settleDownloadNotice('error', '下载失败', err.message || '网络请求失败');
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
      setTasksViewActive(activeCategory === 'tasks');
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

  // ==================== 启动器背景 ====================
  // 内置 4 张 + <启动器目录>\Backgrounds\ 里的自定义图，都由后端
  // background.* 管理（见 Services/BackgroundService.h）。
  // 前端只负责把选中的 url 套到 CSS 变量 --bg-image 上
  const bgMask = document.getElementById('bgMask');
  const bgDialog = document.getElementById('bgDialog');
  // 换底图时的两个辅助层（见 transitionBackground）：
  // bgFade 负责把新图淡进来，bgVeil 负责在这期间把底图柔化
  const bgFade = document.getElementById('bgFade');
  const bgVeil = document.getElementById('bgVeil');

  // 当前背景。启动时先按镜像显示，background.list 回来后再校正
  let currentBackground = {
    key: 'pic1',
    name: '流萤幻界',
    url: 'https://tauricpp.app/resoures/background/pic1.svg'
  };
  Object.assign(currentBackground, readBackgroundMirror());

  function setCurrentBackground(background) {
    if (!background || !background.url) return;
    currentBackground = {
      key: background.key || currentBackground.key,
      name: background.name || currentBackground.name,
      url: background.url
    };
    // 页面所有背景都由 :root 的 --bg-image 驱动（见 style.css），
    // 改这个变量就等于换底图
    document.documentElement.style.setProperty('--bg-image', `url("${currentBackground.url}")`);
    try {
      window.localStorage.setItem(BG_MIRROR_KEY, JSON.stringify(currentBackground));
    } catch (error) {
      // 隐私模式之类写不进去就只是丢了首帧镜像，不影响功能
    }

    // 设置页里的名字可能要跟着改
    const label = document.getElementById('bgCurrentLabel');
    if (label) label.textContent = currentBackground.name;
  }

  // 后端是唯一权威：配置读出来以后覆盖镜像值。
  // 脚本求值阶段和 load 之后各同步一次 —— 太早发起的 invokes 有可能在
  // 页面/桥接还没完全就绪时丢掉，那正是"重启后背景还是默认图"的成因。
  // 这个同步是幂等的，重复调用只会把同一个值再写一次 CSS 变量
  function syncBackgroundFromBackend() {
    tauri.invoke('background.list').then((result) => {
      if (result && result.ok && result.current) setCurrentBackground(result.current);
    }).catch(() => {});
  }
  syncBackgroundFromBackend();
  window.addEventListener('load', syncBackgroundFromBackend);

  function closeBackgroundDialog() {
    if (!bgMask) return;
    bgMask.hidden = true;
    bgDialog.innerHTML = '';
  }

  // 每次打开都重新拉一次清单：用户很可能刚往 Backgrounds 里丢了图
  function openBackgroundDialog() {
    if (!bgMask || !bgDialog) return;
    bgMask.hidden = false;
    bgDialog.innerHTML = `
      <div class="bg-dialog-loading">
        <div class="download-spinner"></div>
        <p>正在读取背景列表...</p>
      </div>
    `;

    tauri.invoke('background.list').then((result) => {
      if (!result || !result.ok) {
        bgDialog.innerHTML = '<div class="bg-empty">读取背景列表失败</div>';
        return;
      }
      renderBackgroundDialog(result);
    }).catch(() => {
      bgDialog.innerHTML = '<div class="bg-empty">读取背景列表失败</div>';
    });
  }

  function buildBackgroundCard(item, activeKey) {
    const card = document.createElement('button');
    card.type = 'button';
    card.className = 'bg-card' + (item.key === activeKey ? ' is-active' : '');
    card.dataset.key = item.key;
    card.innerHTML = `
      <span class="bg-card-thumb" style="background-image:url('${item.url}')"></span>
      <span class="bg-card-name">${escapeHtml(item.name)}</span>
    `;
    return card;
  }

  function renderBackgroundDialog(result) {
    const activeKey = (result.current && result.current.key) || currentBackground.key;
    const builtins = result.builtins || [];
    const custom = result.custom || [];

    bgDialog.innerHTML = `
      <div class="bg-dialog-title">
        <span>背景</span>
        <span class="bg-dialog-sub">${builtins.length + custom.length} 张可用</span>
      </div>
    `;

    // 卡片区域单独滚动：底部那排操作（打开文件夹 / 完成）始终留在视野里，
    // 自定义图多了也不会把它们顶出弹窗
    const body = document.createElement('div');
    body.className = 'bg-dialog-body';

    // ---- 内置 ----
    const builtinLabel = document.createElement('div');
    builtinLabel.className = 'bg-section-label';
    builtinLabel.textContent = '内置';
    body.appendChild(builtinLabel);

    const builtinGrid = document.createElement('div');
    builtinGrid.className = 'bg-grid';
    builtins.forEach(item => builtinGrid.appendChild(buildBackgroundCard(item, activeKey)));
    body.appendChild(builtinGrid);

    // ---- 自定义 ----
    const customLabel = document.createElement('div');
    customLabel.className = 'bg-section-label';
    customLabel.textContent = '自定义';
    body.appendChild(customLabel);

    if (custom.length === 0) {
      const empty = document.createElement('div');
      empty.className = 'bg-empty';
      empty.textContent = '还没有自定义底图。把图片放进下面的 Backgrounds 文件夹，再点刷新就会出现在这里。';
      body.appendChild(empty);
    } else {
      const customGrid = document.createElement('div');
      customGrid.className = 'bg-grid';
      custom.forEach(item => customGrid.appendChild(buildBackgroundCard(item, activeKey)));
      body.appendChild(customGrid);
    }

    bgDialog.appendChild(body);

    // ---- 固定底部：文件夹操作 + 完成 ----
    const footer = document.createElement('div');
    footer.className = 'bg-dialog-footer';
    footer.innerHTML = `
      <div class="bg-folder-row">
        <button type="button" class="bg-btn is-secondary" id="bgOpenFolder">打开 Backgrounds 文件夹</button>
        <button type="button" class="bg-btn is-secondary" id="bgRefresh">刷新</button>
      </div>
      <div class="bg-dialog-hint">支持 jpg / png / bmp / gif / webp / avif，文件名就是卡片名</div>
      <button type="button" class="bg-btn" id="bgClose">完成</button>
    `;
    bgDialog.appendChild(footer);

    bgDialog.querySelectorAll('.bg-card').forEach(card => {
      card.addEventListener('click', () => selectBackground(card.dataset.key));
    });

    bgDialog.querySelector('#bgOpenFolder').addEventListener('click', () => {
      tauri.invoke('background.openFolder').then((res) => {
        if (!res || !res.ok) {
          showNotification({
            title: '打开失败',
            content: '无法打开 Backgrounds 文件夹',
            type: 'error',
            duration: 5000
          });
        }
      }).catch(() => {});
    });
    bgDialog.querySelector('#bgRefresh').addEventListener('click', () => openBackgroundDialog());
    bgDialog.querySelector('#bgClose').addEventListener('click', () => closeBackgroundDialog());
  }

  // 点遮罩关掉弹窗：只挂一次，别跟着每次重绘叠加
  if (bgMask) {
    bgMask.addEventListener('click', (event) => {
      if (event.target === bgMask) closeBackgroundDialog();
    });
  }

  // 换底图的整段动画（约 1080ms）：界面退场 → 背景在模糊里交叉渐变 →
  // 界面从模糊里浮回来 → 收模糊。
  //
  // 四段是**交叠**的，不是接力：每一段还没收尾，下一段就已经起手。
  // 之前按"上一段跑完才起下一段"排，每段收尾时画面都会定住一瞬，
  // 四个节拍就散成了四段独立动画。
  // 唯一必须保持的先后是"收模糊"排在"界面回来"之后 —— 否则会出现
  // 整屏清晰、界面还没落位的那一帧。
  const BG_CROSSFADE_MS = 440;   // 与 .bg-fade 的 opacity 0.44s 对齐
  const BG_FOCUS_MS = 380;       // 与 .bg-veil 的 backdrop-filter 0.38s 对齐
  // 界面淡出/淡入的 0.32s 写在 .is-bg-fading 里，这里只需要知道它的起手时刻

  const BG_CROSSFADE_AT = 120;   // 界面刚退到小半就开始换图
  const BG_RETURN_AT = 380;      // 渐变尾声界面已经在回来了
  // 新图盖满的那一刻。真正的 --bg-image 必须等到这里才换：
  // 渐变层那时才 100% 不透明，底下的图当场变掉是看不见的；
  // 提前到界面回来的 380ms 换，会正好在半透明时露馅
  const BG_SWAP_AT = BG_CROSSFADE_AT + BG_CROSSFADE_MS;
  const BG_END_MS = BG_SWAP_AT + BG_FOCUS_MS;
  let bgSwitching = false;

  function transitionBackground(background, onSwapped) {
    if (bgSwitching || !background || !background.url) return;

    // 跟随系统"减少动效"时不做这套编排，直接换
    if (motionQuery.matches) {
      if (onSwapped) onSwapped();
      setCurrentBackground(background);
      return;
    }

    bgSwitching = true;
    bgFade.style.backgroundImage = `url("${background.url}")`;
    document.body.classList.add('is-bg-fading', 'is-bg-receded', 'is-bg-locked');
    // 底图先柔化：这一层盖在界面之上，所以界面也是"化进模糊里"的。
    // 先上 is-active 把 display:none 换成 block 并落一帧 blur(0px) 基线，
    // 否则从 display:none 直接加 is-blurred 是没有起点可插值的，模糊会跳出来
    bgVeil.classList.add('is-active');
    void bgVeil.offsetWidth;
    bgVeil.classList.add('is-blurred');

    // 界面还没退干净就起手换图：等到全透明再开始的话，中间会空一拍
    setTimeout(() => bgFade.classList.add('is-visible'), BG_CROSSFADE_AT);

    // 渐变还没走完就让界面回来。此时底图已经很软，界面在模糊里浮出，
    // 和渐变的收尾叠在一起，不会读成"图换完了，然后界面出现"
    setTimeout(() => {
      if (onSwapped) onSwapped();
      document.body.classList.remove('is-bg-receded');
    }, BG_RETURN_AT);

    // 新图完全盖住旧图：这时候换底图 + 收模糊。画面已被 UI 盖住，
    // 变清晰的只有 UI 没盖住的缝隙，玻璃面板后面由它们自己的模糊撑着
    setTimeout(() => {
      setCurrentBackground(background);
      bgVeil.classList.remove('is-blurred');
    }, BG_SWAP_AT);

    // 收尾。过渡层此时和 body 背景是同一张图，淡出它不会有任何视觉变化
    setTimeout(() => {
      bgFade.classList.remove('is-visible');
      bgVeil.classList.remove('is-active');
      document.body.classList.remove('is-bg-fading', 'is-bg-locked');
      bgSwitching = false;
    }, BG_END_MS);
  }

  function selectBackground(key) {
    if (bgSwitching) return;

    tauri.invoke('background.select', { key }).then((result) => {
      if (!result || !result.ok) {
        showNotification({
          title: '切换背景失败',
          content: (result && result.error) || '切换背景失败',
          type: 'error',
          duration: 5000
        });
        return;
      }
      // 后端存下来了但没给出可用的地址：以前这里会静默什么都不做，
      // 看起来就像点了没反应。宁可报出来
      if (!result.current || !result.current.url) {
        showNotification({
          title: '切换背景失败',
          content: '后端没有返回可用的背景地址',
          type: 'error',
          duration: 5000
        });
        return;
      }

      // 先把点中的卡片标成选中，界面退场前的这 180ms 里有即时反馈
      bgDialog.querySelectorAll('.bg-card').forEach(card => {
        card.classList.toggle('is-active', card.dataset.key === key);
      });
      transitionBackground(result.current, () => closeBackgroundDialog());
    }).catch(() => {
      showNotification({ title: '切换背景失败', content: '切换背景失败', type: 'error', duration: 5000 });
    });
  }

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

      // 背景：右边显示当前底图名字，点开是卡片选择弹窗
      const bgRow = document.createElement('div');
      bgRow.className = 'settings-item';
      bgRow.innerHTML = `
        <span class="settings-item-label">背景</span>
        <span class="settings-item-value" id="bgCurrentLabel">${escapeHtml(currentBackground.name)}</span>
      `;
      bgRow.addEventListener('click', () => openBackgroundDialog());
      wrapper.appendChild(bgRow);

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
      title2.textContent = '游戏设置';
      wrapper.appendChild(title2);

      wrapper.appendChild(buildGameSettingsSection());

      const title3 = document.createElement('div');
      title3.className = 'settings-section-title';
      title3.textContent = 'Java 配置';
      wrapper.appendChild(title3);

      wrapper.appendChild(buildJavaRow());

      settingsContent.appendChild(wrapper);
    });
  }

  // 游戏设置：内存 / 窗口大小 / 版本隔离，全部由 config.getGameSettings + setGameSettings 读写
  function buildGameSettingsSection() {
    const section = document.createElement('div');

    const HINT_STYLE = 'font-size:12px;color:rgba(255,255,255,0.5);margin:-2px 0 6px;line-height:1.5;';
    const INPUT_STYLE = 'width:84px;padding:6px 10px;border:1px solid rgba(255,255,255,0.24);border-radius:6px;font-size:13px;background:rgba(255,255,255,0.12);color:#fff;outline:none;';

    const WINDOW_MODES = [
      { id: 'default', name: '默认（854 × 480）' },
      { id: 'fullscreen', name: '全屏' },
      { id: 'custom', name: '自定义' }
    ];
    const ISOLATION_MODES = [
      { id: 'off', name: '关闭（所有版本共用 .minecraft）' },
      { id: 'modded', name: '仅隔离模组版本' },
      { id: 'all', name: '隔离全部版本' }
    ];
    // 首次启动写入 options.txt 的语言；游戏本体只认这种 <lang>_<region> 小写代码
    const LANGUAGES = [
      { id: 'zh_cn', name: '简体中文' },
      { id: 'zh_tw', name: '繁體中文' },
      { id: 'en_us', name: 'English (US)' },
      { id: 'en_gb', name: 'English (UK)' },
      { id: 'ja_jp', name: '日本語' },
      { id: 'ko_kr', name: '한국어' },
      { id: 'fr_fr', name: 'Français' },
      { id: 'de_de', name: 'Deutsch' },
      { id: 'es_es', name: 'Español' },
      { id: 'ru_ru', name: 'Русский' },
      { id: 'pt_br', name: 'Português (Brasil)' },
      { id: 'it_it', name: 'Italiano' },
      { id: 'pl_pl', name: 'Polski' },
      { id: 'nl_nl', name: 'Nederlands' },
      { id: 'tr_tr', name: 'Türkçe' },
      { id: 'uk_ua', name: 'Українська' }
    ];

    const buildOptionRow = (label, options, currentValue, onPick) => {
      const row = document.createElement('div');
      row.className = 'select-row';
      row.innerHTML = `<span class="select-row-label">${label}</span>`;

      const select = document.createElement('select');
      options.forEach(opt => {
        const o = document.createElement('option');
        o.value = opt.id;
        o.textContent = opt.name;
        if (opt.id === currentValue) o.selected = true;
        select.appendChild(o);
      });
      select.addEventListener('change', () => onPick(select.value));

      row.appendChild(select);
      return row;
    };

    const hint = (text) => {
      const el = document.createElement('div');
      el.style.cssText = HINT_STYLE;
      el.textContent = text;
      return el;
    };

    tauri.invoke('config.getGameSettings').then((state) => {
      const settings = {
        maxMemoryMb: (state && state.maxMemoryMb) || 0,
        windowMode: (state && state.windowMode) || 'default',
        windowWidth: (state && state.windowWidth) || 854,
        windowHeight: (state && state.windowHeight) || 480,
        versionIsolation: (state && state.versionIsolation) || 'off',
        language: (state && state.language) || 'zh_cn'
      };
      const autoMemoryMb = (state && state.autoMemoryMb) || 2048;
      const totalMemoryMb = (state && state.totalMemoryMb) || 8192;
      const save = () => tauri.invoke('config.setGameSettings', settings);

      // ---- 游戏语言 ----
      section.appendChild(buildOptionRow('游戏语言', LANGUAGES, settings.language, (value) => {
        settings.language = value;
        save();
      }));
      section.appendChild(hint('首次启动（该实例还没有存档）时写入 options.txt 的语言，并跳过无障碍引导界面；已经有存档的实例不会被改动'));

      // ---- 游戏内存 ----
      const maxGb = Math.max(1, Math.floor(totalMemoryMb / 1024));
      const sliderValue = settings.maxMemoryMb > 0 ? settings.maxMemoryMb / 1024 : 0;
      const label = () => settings.maxMemoryMb > 0
        ? `${(settings.maxMemoryMb / 1024).toFixed(1)} GB`
        : `自动（${(autoMemoryMb / 1024).toFixed(1)} GB）`;

      const ramRow = document.createElement('div');
      ramRow.className = 'slider-row';
      ramRow.innerHTML = `
        <div class="slider-row-header">
          <span class="slider-row-label">游戏内存</span>
          <span class="slider-row-value">${label()}</span>
        </div>
        <input type="range" min="0" max="${maxGb}" step="0.5" value="${sliderValue}">
        <div class="slider-warning">拉到最左侧为自动分配（按可用内存与该版本的模组数量）</div>
      `;
      const ramInput = ramRow.querySelector('input[type="range"]');
      const ramValue = ramRow.querySelector('.slider-row-value');
      ramInput.addEventListener('input', () => {
        const gb = parseFloat(ramInput.value);
        settings.maxMemoryMb = gb <= 0 ? 0 : Math.round(gb * 1024);
        ramValue.textContent = label();
        save();
      });
      section.appendChild(ramRow);
      section.appendChild(hint(`物理内存 ${(totalMemoryMb / 1024).toFixed(1)} GB；自动值按当前可用内存计算，会随系统负载变化`));

      // ---- 窗口大小 ----
      const windowWrap = document.createElement('div');
      const renderWindow = () => {
        windowWrap.innerHTML = '';
        windowWrap.appendChild(buildOptionRow('窗口大小', WINDOW_MODES, settings.windowMode, (value) => {
          settings.windowMode = value;
          save();
          renderWindow();
        }));

        if (settings.windowMode === 'custom') {
          const sizeRow = document.createElement('div');
          sizeRow.className = 'select-row';
          sizeRow.innerHTML = `<span class="select-row-label">分辨率</span>`;

          const controls = document.createElement('div');
          controls.style.cssText = 'display:flex;align-items:center;gap:8px;';

          const widthInput = document.createElement('input');
          widthInput.type = 'number';
          widthInput.min = '100';
          widthInput.max = '10000';
          widthInput.value = settings.windowWidth;
          widthInput.style.cssText = INPUT_STYLE;

          const times = document.createElement('span');
          times.textContent = '×';
          times.style.color = 'rgba(255,255,255,0.68)';

          const heightInput = document.createElement('input');
          heightInput.type = 'number';
          heightInput.min = '100';
          heightInput.max = '10000';
          heightInput.value = settings.windowHeight;
          heightInput.style.cssText = INPUT_STYLE;

          const commit = () => {
            const w = parseInt(widthInput.value, 10) || 854;
            const h = parseInt(heightInput.value, 10) || 480;
            settings.windowWidth = Math.max(100, Math.min(10000, w));
            settings.windowHeight = Math.max(100, Math.min(10000, h));
            widthInput.value = settings.windowWidth;
            heightInput.value = settings.windowHeight;
            save();
          };
          widthInput.addEventListener('change', commit);
          heightInput.addEventListener('change', commit);

          controls.appendChild(widthInput);
          controls.appendChild(times);
          controls.appendChild(heightInput);
          sizeRow.appendChild(controls);
          windowWrap.appendChild(sizeRow);
        }
      };
      renderWindow();
      section.appendChild(windowWrap);

      // ---- 版本隔离 ----
      section.appendChild(buildOptionRow('版本隔离', ISOLATION_MODES, settings.versionIsolation, (value) => {
        settings.versionIsolation = value;
        save();
      }));
      section.appendChild(hint('开启后每个版本用独立的存档、配置与 mods 目录；关闭则所有版本共用 .minecraft'));

      // 让包裹层不参与布局，行的间距与直接挂在 .settings-section 下一致
      section.style.display = 'contents';
    }, () => {
      section.appendChild(hint('游戏设置加载失败'));
    });

    return section;
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

  // Java 选择栏：下拉框第一项固定是「自动选择」，右侧小按钮重新扫描
  const AUTO_JAVA_VALUE = '__auto__';
  // java.scan 是全盘扫描，很慢。结果缓存下来，切页 / 换设置分类再回来时直接复用，
  // 只有点刷新按钮才真正重新扫描。
  let javaScanCache = null;
  const JAVA_REFRESH_ICON = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="23 4 23 10 17 10"/><polyline points="1 20 1 14 7 14"/><path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"/></svg>';

  function buildJavaRow() {
    const row = document.createElement('div');
    row.className = 'select-row';
    row.innerHTML = `<span class="select-row-label">Java</span>`;

    const controls = document.createElement('div');
    controls.className = 'select-row-controls';

    const select = document.createElement('select');
    select.className = 'java-select';
    select.disabled = true;

    const refreshBtn = document.createElement('button');
    refreshBtn.type = 'button';
    refreshBtn.className = 'select-row-btn';
    refreshBtn.title = '重新扫描 Java';
    refreshBtn.setAttribute('aria-label', '重新扫描 Java');
    refreshBtn.innerHTML = JAVA_REFRESH_ICON;

    controls.appendChild(select);
    controls.appendChild(refreshBtn);
    row.appendChild(controls);

    let lastState = null;

    const render = (state) => {
      const javas = (state && Array.isArray(state.javas)) ? state.javas : [];
      select.innerHTML = '';

      // 自动选择永远占据第一位
      const autoOption = document.createElement('option');
      autoOption.value = AUTO_JAVA_VALUE;
      autoOption.textContent = (state && state.autoPath)
        ? `自动选择（Java ${state.autoMajor}） · ${state.autoPath}`
        : '自动选择（未找到可用 Java）';
      select.appendChild(autoOption);

      javas.forEach(java => {
        const option = document.createElement('option');
        option.value = java.path;
        const label = java.version
          ? `Java ${java.major}（${java.version}）`
          : `Java（${java.name}）`;
        option.textContent = `${label} · ${java.path}`;
        select.appendChild(option);
      });

      if (!state || state.auto) {
        select.value = AUTO_JAVA_VALUE;
      } else {
        // 手动选的路径可能已被卸载或换盘，补一条占位项，免得下拉框错位到别的 Java 上
        if (state.selectedPath && !javas.some(java => java.path === state.selectedPath)) {
          const option = document.createElement('option');
          option.value = state.selectedPath;
          option.textContent = '已保存的 Java（未检测到）';
          select.appendChild(option);
        }
        select.value = state.selectedPath || AUTO_JAVA_VALUE;
      }

      // 路径过长时会被宽度截断，用原生提示补全
      const selectedOption = select.options[select.selectedIndex];
      select.title = selectedOption ? selectedOption.textContent : '';
    };

    const apply = (state) => {
      lastState = state;
      render(state);
    };

    const load = (force) => {
      if (!force && javaScanCache) {
        apply(javaScanCache);
        return;
      }

      select.disabled = true;
      refreshBtn.disabled = true;
      refreshBtn.classList.add('spinning');
      select.innerHTML = '<option>正在扫描...</option>';

      const finish = () => {
        select.disabled = false;
        refreshBtn.disabled = false;
        refreshBtn.classList.remove('spinning');
      };

      // 扫描要枚举所有磁盘并逐个跑 java -version，后端已经改成独立线程执行，
      // 这里拿到请求号后轮询结果（不打 alive 守卫：扫描本身要继续跑完，
      // 结果留在缓存里，重新进设置页就能直接复用）
      tauri.invoke('java.scan').then((started) => {
        if (!started || !started.requestId) throw new Error('scan not accepted');
        return pollAsyncResult('java.poll', { requestId: started.requestId }, null, 180000);
      }).then((state) => {
        javaScanCache = state;
        apply(state);
        // 自动选择模式下把最新结果落盘，保证启动时用的就是刚挑出来的 Java
        if (state && state.auto && state.autoPath && state.autoPath !== state.selectedPath) {
          tauri.invoke('java.select', { auto: true, path: state.autoPath });
        }
      }).catch(() => {
        select.innerHTML = '<option>扫描失败</option>';
      }).then(finish);
    };

    select.addEventListener('change', () => {
      const picked = select.value;
      const isAuto = picked === AUTO_JAVA_VALUE;
      const path = isAuto ? ((lastState && lastState.autoPath) || '') : picked;

      select.disabled = true;
      tauri.invoke('java.select', { auto: isAuto, path }).then(() => {
        if (lastState) {
          lastState.auto = isAuto;
          lastState.selectedPath = path;
          render(lastState);
        }
        select.disabled = false;
      }, () => {
        select.disabled = false;
      });
    });

    refreshBtn.addEventListener('click', () => load(true));

    load(false);
    return row;
  }

  // ==================== 资源管理页逻辑 ====================
  // 这一页的副导航栏有两种形态，共用同一根栏：
  //   versions —— 已下载版本列表
  //   instance —— 单个版本的分层管理：游戏设置 / 模组 / 资源包 / 光影包
  // 从列表点「版本设置」进实例时，整条副导航栏退场重进，内容区同时换成
  // 对应的分类。这样几百个 mod 有自己的整页空间，不用挤在版本详情里折叠。
  const resourceContent = document.getElementById('resourceContent');
  const resourceSubSidebar = document.getElementById('resourceSubSidebar');

  let resourceNavMode = 'versions';
  let activeResourceTab = 'versions';
  // instance 模式下正在管理的版本（version.getInstalled 返回的那一项）
  let resourceInstance = null;

  // 固定到主页的版本名（profileName），存在 config.json 的 game.pinnedVersions 里。
  // 主页左下角的版本卡片只列这里的版本，其余一律不显示
  let pinnedVersionNames = [];

  // 固定数量上限，和后端 ConfigManager::kMaxPinnedVersions 保持一致。
  // 后端才是最终把关的，这里只用来提前禁用图钉
  const MAX_PINNED_VERSIONS = 8;

  function isVersionPinned(profileName) {
    return pinnedVersionNames.indexOf(profileName) !== -1;
  }

  const RESOURCE_NAV_ITEMS = {
    versions: [
      { tab: 'versions', label: '版本', icon: '<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/><line x1="16" y1="13" x2="8" y2="13"/><line x1="16" y1="17" x2="8" y2="17"/>' }
    ],
    instance: [
      { tab: 'settings', label: '游戏设置', icon: '<line x1="4" y1="21" x2="4" y2="14"/><line x1="4" y1="10" x2="4" y2="3"/><line x1="12" y1="21" x2="12" y2="12"/><line x1="12" y1="8" x2="12" y2="3"/><line x1="20" y1="21" x2="20" y2="16"/><line x1="20" y1="12" x2="20" y2="3"/><line x1="1" y1="14" x2="7" y2="14"/><line x1="9" y1="8" x2="15" y2="8"/><line x1="17" y1="16" x2="23" y2="16"/>' },
      { tab: 'mods', label: '模组', icon: '<path d="M21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z"/><polyline points="3.27 6.96 12 12.01 20.73 6.96"/><line x1="12" y1="22.08" x2="12" y2="12"/>' },
      { tab: 'resourcepacks', label: '资源包', icon: '<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/>' },
      { tab: 'shaderpacks', label: '光影包', icon: '<circle cx="12" cy="12" r="5"/><line x1="12" y1="1" x2="12" y2="3"/><line x1="12" y1="21" x2="12" y2="23"/><line x1="4.22" y1="4.22" x2="5.64" y2="5.64"/><line x1="18.36" y1="18.36" x2="19.78" y2="19.78"/><line x1="1" y1="12" x2="3" y2="12"/><line x1="21" y1="12" x2="23" y2="12"/><line x1="4.22" y1="19.78" x2="5.64" y2="18.36"/><line x1="18.36" y1="5.64" x2="19.78" y2="4.22"/>' },
      { tab: 'back', label: '返回', icon: '<line x1="19" y1="12" x2="5" y2="12"/><polyline points="12 19 5 12 12 5"/>' }
    ]
  };

  function renderResourceNav() {
    const items = RESOURCE_NAV_ITEMS[resourceNavMode];
    resourceSubSidebar.innerHTML = items.map(item => `
      <button type="button" class="sub-sidebar-item${item.tab === activeResourceTab ? ' active' : ''}" data-tab="${item.tab}">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">${item.icon}</svg>
        <span>${item.label}</span>
      </button>
    `).join('');
    // 实例模式下「返回」要沉到栏底，靠这条类选择器（见 style.css）
    resourceSubSidebar.classList.toggle('is-instance', resourceNavMode === 'instance');
  }

  // 展示区换内容：先淡出（.is-swapping 只给 0.16s，比导航的滑出快），
  // 换完 DOM 再淡回来，新插入的行自己播 stagger-in。
  // 不这么做的话内容会在导航滑到一半时硬切，和周围的动效完全不在一个节奏上
  function swapResourceContent(mutate, outMs) {
    resourceContent.classList.add('is-swapping');
    setTimeout(() => {
      mutate();
      resourceContent.classList.remove('is-swapping');
    }, motionDelay(outMs));
  }

  // 副导航栏退场重进：导航滑出的同时内容一起淡出，
  // 换完导航项和内容后两边再一起进场
  function swapResourceNav(mutate) {
    resourceSubSidebar.classList.remove('slide-in');
    resourceSubSidebar.classList.add('slide-out');
    swapResourceContent(() => {
      mutate();
      resourceSubSidebar.classList.remove('slide-out');
      // 这 180ms 里用户可能已经切到别的页了，那就别再把它滑出来
      if (resourcePage.classList.contains('active')) {
        void resourceSubSidebar.offsetWidth;   // 强制重排，滑入动画才会重播
        resourceSubSidebar.classList.add('slide-in');
      }
    }, 180);
  }

  // 副导航栏点击（内容是动态生成的，所以用事件委托）
  resourceSubSidebar.addEventListener('click', (event) => {
    const item = event.target.closest('.sub-sidebar-item');
    if (!item) return;
    const tab = item.dataset.tab;
    if (tab === 'back') {
      backToResourceVersions();
      return;
    }
    if (tab === activeResourceTab) return;
    activeResourceTab = tab;
    // 导航本身不动（只是高亮换位），所以只让展示区做个短一点的淡出淡入，
    // 否则内容还是硬切，跟刚进实例时的过渡对不上。
    // 160ms 要略大于 .is-swapping 的 0.16s 退场，换 DOM 时才看不见
    renderResourceNav();
    swapResourceContent(renderResourceTab, 160);
  });

  function loadResourceContent() {
    renderResourceNav();
    renderResourceTab();
  }

  function renderResourceTab() {
    if (resourceNavMode === 'versions' || !resourceInstance) {
      loadResourceVersions();
      return;
    }
    if (activeResourceTab === 'settings') {
      renderInstanceSettings(resourceInstance);
    } else {
      renderInstanceFiles(resourceInstance, activeResourceTab);
    }
  }

  // 从版本列表进入实例管理：副导航栏退场重进 + 内容换成「游戏设置」
  function openResourceInstance(v) {
    resourceInstance = v;
    resourceNavMode = 'instance';
    activeResourceTab = 'settings';
    swapResourceNav(() => {
      renderResourceNav();
      renderResourceTab();
    });
  }

  function backToResourceVersions() {
    resourceInstance = null;
    resourceNavMode = 'versions';
    activeResourceTab = 'versions';
    swapResourceNav(() => {
      renderResourceNav();
      // 列表要重拉一遍：刚在实例里删过版本的话这里得反映出来
      loadResourceVersions();
    });
  }

  // 实例页统一的小标题条：左边版本名（导航栏里看不到是哪个版本），右边当前分类
  function buildInstanceHead(label) {
    const head = document.createElement('div');
    head.className = 'resource-instance-head';
    head.innerHTML = `
      <span class="resource-instance-name">${escapeHtml(resourceInstance.profileName)}</span>
      <span class="resource-instance-tab">${label}</span>
    `;
    return head;
  }

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
        pinnedVersionNames = (result.pinned || []).slice();
        result.versions.forEach(v => wrapper.appendChild(buildVersionManageRow(v)));
      } else {
        const empty = document.createElement('div');
        empty.className = 'settings-item';
        empty.innerHTML = `<span class="settings-item-label" style="color:rgba(255,255,255,0.68);">暂无已下载版本</span>`;
        wrapper.appendChild(empty);
      }

      resourceContent.innerHTML = '';
      resourceContent.appendChild(wrapper);
    }).catch(() => {
      resourceContent.innerHTML = `<div class="settings-section"><p style="color:#fff;">加载失败</p></div>`;
    });
  }

  // 一行已下载版本：左侧只是信息（不可点），右侧三个按钮——
  // 启动游戏直接开这个版本，扳手进版本详情页，图钉决定它是否出现在主页左下角的版本卡片里
  function buildVersionManageRow(v) {
    const row = document.createElement('div');
    row.className = 'settings-item version-manage-row';
    row.innerHTML = `
      <span class="version-manage-info">
        <span class="settings-item-label">${v.profileName}</span>
        <span class="settings-item-value">${v.actualVersionId || ''}</span>
      </span>
      <span class="version-manage-actions">
        <button type="button" class="version-manage-launch">启动游戏</button>
        <button type="button" class="version-manage-btn version-manage-setup" title="版本设置">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.77-3.77a6 6 0 0 1-7.94 7.94l-6.91 6.91a2.12 2.12 0 0 1-3-3l6.91-6.91a6 6 0 0 1 7.94-7.94l-3.76 3.76z"/></svg>
        </button>
        <button type="button" class="version-manage-btn version-manage-pin">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 17v5"/><path d="M9 10.76a2 2 0 0 1-1.11 1.79l-1.78.9A2 2 0 0 0 5 15.24V16a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-.76a2 2 0 0 0-1.11-1.79l-1.78-.9A2 2 0 0 1 15 10.76V7h1a2 2 0 0 0 0-4H8a2 2 0 0 0 0 4h1z"/></svg>
        </button>
      </span>
    `;

    const launchBtn = row.querySelector('.version-manage-launch');
    const pinBtn = row.querySelector('.version-manage-pin');
    // 图钉的状态要按行查固定列表，所以把版本名挂在按钮上（refreshPinButtons 也靠它）
    pinBtn.dataset.version = v.profileName;
    applyPinState(pinBtn, v.profileName);

    launchBtn.addEventListener('click', () => launchVersionFromList(v.profileName, launchBtn));
    row.querySelector('.version-manage-setup').addEventListener('click', () => openResourceInstance(v));

    pinBtn.addEventListener('click', () => {
      const next = !isVersionPinned(v.profileName);
      pinBtn.disabled = true;
      tauri.invoke('version.togglePinned', { profileName: v.profileName, pinned: next }).then((result) => {
        if (!result || !result.ok) {
          showNotification({
            title: '固定失败',
            content: (result && result.error) || '固定失败',
            type: 'error',
            duration: 5000
          });
          refreshPinButtons();
          return;
        }
        pinnedVersionNames = pinnedVersionNames.filter(name => name !== v.profileName);
        if (result.pinned) pinnedVersionNames.push(v.profileName);
        // 固定数量变了，其它行的图钉可用状态要一起重算
        refreshPinButtons();
        // 主页卡片的列表读的是同一份固定列表，这里改完顺手刷新
        loadVersionCard();
      }).catch(() => {
        showNotification({ title: '固定失败', content: '固定失败', type: 'error', duration: 5000 });
        refreshPinButtons();
      });
    });

    return row;
  }

  // 是否还能再固定一个：到上限后未固定的那些图钉直接禁用，省得点了才报错
  function applyPinState(btn, profileName) {
    const pinned = isVersionPinned(profileName);
    const atLimit = !pinned && pinnedVersionNames.length >= MAX_PINNED_VERSIONS;
    btn.classList.toggle('is-pinned', pinned);
    btn.disabled = atLimit;
    btn.title = pinned ? '取消固定'
      : atLimit ? `最多只能固定 ${MAX_PINNED_VERSIONS} 个版本` : '固定到主页';
  }

  function refreshPinButtons() {
    resourceContent.querySelectorAll('.version-manage-pin').forEach(btn => {
      applyPinState(btn, btn.dataset.version);
    });
  }

  // 实例的「游戏设置」页：版本信息 + 删除。mod / 资源包 / 光影包
  // 各自有独立分页，这里只放跟这个版本本身有关的静态信息
  function renderInstanceSettings(v) {
    resourceContent.innerHTML = '';
    const wrapper = document.createElement('div');
    wrapper.className = 'settings-section';

    wrapper.appendChild(buildInstanceHead('游戏设置'));

    const info = [
      { label: '版本名称', value: v.profileName },
      { label: '实际版本', value: v.actualVersionId || '未知' },
      { label: '主类', value: v.mainClass || '未知' }
    ];
    info.forEach(item => {
      const row = document.createElement('div');
      row.className = 'settings-item';
      row.style.cursor = 'default';
      row.innerHTML = `
        <span class="settings-item-label">${item.label}</span>
        <span class="settings-item-value">${escapeHtml(item.value)}</span>
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
            backToResourceVersions();
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

  // 三个分页各自的文案。目录名同时也是后端 game.listMods 的 category
  const CONTENT_CATEGORY_TEXT = {
    mods: {
      label: '模组',
      loading: '正在读取 mods 文件夹...',
      empty: 'mods 文件夹是空的',
      shared: '当前未开启版本隔离，所有版本共用 .minecraft\\mods，这里的改动会影响其它版本'
    },
    resourcepacks: {
      label: '资源包',
      loading: '正在读取 resourcepacks 文件夹...',
      empty: 'resourcepacks 文件夹是空的',
      shared: '当前未开启版本隔离，所有版本共用 .minecraft\\resourcepacks，这里的改动会影响其它版本'
    },
    shaderpacks: {
      label: '光影包',
      loading: '正在读取 shaderpacks 文件夹...',
      empty: 'shaderpacks 文件夹是空的',
      shared: '当前未开启版本隔离，所有版本共用 .minecraft\\shaderpacks，这里的改动会影响其它版本'
    }
  };

  // 模组 / 资源包 / 光影包分页：整页铺开，文件多了由内容面板自己滚动，
  // 不再折叠——这也是把它们从版本详情里拆出来的原因
  function renderInstanceFiles(v, category) {
    const text = CONTENT_CATEGORY_TEXT[category] || CONTENT_CATEGORY_TEXT.mods;

    resourceContent.innerHTML = '';
    const wrapper = document.createElement('div');
    wrapper.className = 'settings-section';
    wrapper.appendChild(buildInstanceHead(text.label));

    const body = document.createElement('div');
    body.innerHTML = `
      <div class="download-loading" style="height:auto;padding:14px;">
        <div class="download-spinner"></div>
        <p>${text.loading}</p>
      </div>
    `;
    wrapper.appendChild(body);
    resourceContent.appendChild(wrapper);

    tauri.invoke('game.listMods', { version: v.profileName, category }).then((result) => {
      // 请求回来时用户可能已经切到别的分页/退出实例了，别往已经换掉的面板里写
      if (!body.isConnected) return;
      renderFileList(body, v.profileName, category, result);
    }).catch(() => {
      if (!body.isConnected) return;
      body.innerHTML = `<div class="settings-item"><span class="settings-item-label">读取 ${category} 文件夹失败</span></div>`;
    });
  }

  function renderFileList(container, versionName, category, result) {
    const text = CONTENT_CATEGORY_TEXT[category] || CONTENT_CATEGORY_TEXT.mods;
    container.innerHTML = '';
    // 加载态到结果不该是硬切：这里补一次淡入，正常当场返回时它会和面板的进场叠在一起
    container.classList.add('is-revealing');

    if (!result || !result.ok) {
      container.innerHTML = `<div class="settings-item"><span class="settings-item-label">读取 ${category} 文件夹失败</span></div>`;
      return;
    }

    // 没开版本隔离时多个版本共用同一个目录，得说清楚
    if (result.shared) {
      const shared = document.createElement('div');
      shared.className = 'settings-item-hint';
      shared.textContent = text.shared;
      container.appendChild(shared);
    }

    const files = result.files || [];
    if (files.length === 0) {
      const empty = document.createElement('div');
      empty.className = 'settings-item';
      empty.style.cursor = 'default';
      empty.innerHTML = `<span class="settings-item-label" style="color:rgba(255,255,255,0.68);">${text.empty}</span>`;
      container.appendChild(empty);
      return;
    }

    // 启用的排前面，其余按文件名排
    const sorted = files.slice().sort(
      (a, b) => (Number(b.enabled) - Number(a.enabled)) || a.enabledName.localeCompare(b.enabledName)
    );
    const enabledCount = sorted.filter(file => file.enabled).length;

    const summary = document.createElement('div');
    summary.className = 'resource-file-summary';
    summary.textContent = `共 ${sorted.length} 个，已启用 ${enabledCount} 个`;
    container.appendChild(summary);

    // 几百个文件一次建 DOM 也扛得住；容器本身有滚动，不做分页
    const list = document.createElement('div');
    list.className = 'resource-file-list';
    sorted.forEach(file => list.appendChild(buildFileRow(versionName, category, file)));
    container.appendChild(list);
  }

  function buildFileRow(versionName, category, file) {
    const row = document.createElement('div');
    row.className = 'settings-item mod-toggle-row';

    const render = () => {
      row.innerHTML = `
        <span class="mod-toggle-state ${file.enabled ? 'is-on' : 'is-off'}">${file.enabled ? '已启用' : '已停用'}</span>
        <span class="settings-item-label mod-toggle-name" title="${escapeHtml(file.name)}">${escapeHtml(file.enabledName)}</span>
        <span class="settings-item-value">${file.enabled ? '点击停用' : '点击启用'}</span>
      `;
    };
    render();

    row.addEventListener('click', () => {
      row.style.opacity = '0.5';
      tauri.invoke('game.toggleMod', {
        version: versionName,
        category,
        name: file.name,
        enable: !file.enabled
      }).then((res) => {
        row.style.opacity = '';
        if (!res || !res.ok) {
          showNotification({
            title: '切换失败',
            content: (res && res.error) || '切换失败',
            type: 'error',
            duration: 5000
          });
          return;
        }
        file.enabled = res.enabled;
        file.name = res.name;
        file.enabledName = res.enabled ? res.name : res.name.replace(/\.disabled$/, '');
        render();
      }).catch(() => {
        row.style.opacity = '';
        showNotification({ title: '切换失败', content: '切换失败', type: 'error', duration: 5000 });
      });
    });

    return row;
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
      <span class="settings-item-label" style="color:rgba(255,255,255,0.68);font-size:12px;line-height:1.5;">
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
          style="flex:1;padding:6px 10px;border:1px solid rgba(255,255,255,0.24);border-radius:6px;font-size:13px;background:rgba(255,255,255,0.12);color:#fff;outline:none;">
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
              <span class="settings-item-value" style="color:#fca5a5;cursor:pointer;padding:2px 8px;border-radius:4px;background:rgba(239,68,68,0.1);white-space:nowrap;">移除</span>
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
          empty.innerHTML = `<span class="settings-item-label" style="color:rgba(255,255,255,0.68);">暂无忽略项</span>`;
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

  // accountTypes 是后端新增的并行数组，老配置里没有时按离线处理
  function accountTypeLabel(config, index) {
    const types = config && config.accountTypes;
    const type = types && types[index];
    if (type === 'msa') return '正版';
    if (type === 'yggdrasil') return '外置登录';
    return '离线';
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
        config.accounts.forEach((id, i) => {
          const item = document.createElement('div');
          item.className = 'settings-item';
          if (id && id.length > 0) {
            // 区分正版 / 离线：两者都能启动游戏，但身份和服端校验完全不同
            const typeLabel = accountTypeLabel(config, i);
            item.innerHTML = `
              <span class="settings-item-label">${id}</span>
              <span class="settings-item-value">${typeLabel} · 槽位 ${i + 1}</span>
              <span class="settings-item-arrow">&#9656;</span>
            `;
            item.addEventListener('click', () => showAccountDetail(i, id, typeLabel));
          } else {
            item.innerHTML = `
              <span class="settings-item-label" style="color:rgba(255,255,255,0.68);">空槽位 ${i + 1}</span>
              <span class="settings-item-value">未登录</span>
            `;
          }
          wrapper.appendChild(item);
        });
      }
      settingsContent.appendChild(wrapper);
    });
  }

  function showAccountDetail(index, id, typeLabel) {
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
      { label: '登录方式', value: typeLabel || '离线' },
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
  const accountCardAvatarImg = document.getElementById('accountCardAvatarImg');
  const accountCardName = document.getElementById('accountCardName');
  const accountCardExpand = document.getElementById('accountCardExpand');
  const accountCardList = document.getElementById('accountCardList');
  const accountManageBtn = document.getElementById('accountManageBtn');
  const accountCardMain = document.getElementById('accountCardMain');
  let currentAccountIndex = -1;

  // 头像槽：有账户显示角色头像，没有则回到画出来的问号
  function setAccountAvatar(id) {
    if (id) {
      accountCardAvatarImg.src = loginIcon;
      accountCardAvatar.classList.add('has-avatar');
    } else {
      accountCardAvatar.classList.remove('has-avatar');
    }
  }

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
            setAccountAvatar(config.accounts[i]);
            accountCardName.textContent = config.accounts[i];
            currentAccountIndex = i;
            foundAccount = true;
            break;
          }
        }
        if (!foundAccount) {
          setAccountAvatar('');
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
    setAccountAvatar(name);
    accountCardName.textContent = name;
    // 记住选择：启动游戏时按这个槽位取账号与令牌
    tauri.invoke('account.select', { index });

    // 更新列表中的活动状态
    const items = accountCardList.querySelectorAll('.account-list-item');
    items.forEach((item, i) => {
      item.classList.toggle('active', i === index);
    });
  }

  // 管理账户按钮点击事件
  accountManageBtn.addEventListener('click', () => {
    // 回到登录界面，从离线登录开始
    loginInput.value = '';
    selectLoginMethod('offline');

    // 保留已有账户再进登录界面：这里只是“再登一个 / 换一个”，
    // 点错了可以直接用返回退回大厅，账户不会被清掉
    showLayer(layerLogin, true);
  });

  // 返回按钮：放弃这次登录，直接退回大厅（账户数据原样保留）
  loginBack.addEventListener('click', () => {
    // 把等待中的授权收掉，免得后台线程继续轮询
    if (msaState === 'requesting' || msaState === 'waiting') cancelMsaLogin(true);
    else msaState = 'idle';

    showLayer(layerLobby);
    loadAccountCard();
  });

  // 初始化加载账户卡片
  loadAccountCard();

  // ==================== 版本选择卡片逻辑 ====================
  const versionCard = document.getElementById('versionCard');
  const versionCardName = document.getElementById('versionCardName');
  const versionCardList = document.getElementById('versionCardList');
  const versionCardMain = document.getElementById('versionCardMain');
  let currentVersionName = null;
  // 有没有下载过任何版本：卡片里一个固定版本都没有时，靠它决定把人带去下载页还是文件管理页
  let hasInstalledVersions = false;

  // 点击版本卡片主体：版本列表靠鼠标悬浮就会展开，点击不再负责撑开卡片，
  // 所以有固定版本时点击一律把人带去文件管理页管版本
  versionCardMain.addEventListener('click', () => {
    // 一个固定版本都没有时卡片里没得选：连版本都没装过就先去下载
    if (pinnedVersionNames.length === 0) {
      if (!hasInstalledVersions) {
        switchPageWithSidebar('download');
        return;
      }
      showNotification({
        title: '请固定任何一个版本',
        content: '在「资源管理 - 版本」里点右侧图钉，固定后的版本才会出现在主页',
        type: 'warning',
        duration: 6000
      });
    }

    switchPageWithSidebar('resource');
  });

  // 加载版本信息
  function loadVersionCard() {
    tauri.invoke('version.getInstalled').then((result) => {
      if (result && result.ok && result.versions && result.versions.length > 0) {
        hasInstalledVersions = true;
        pinnedVersionNames = (result.pinned || []).slice();

        // 只列固定过的版本，没固定的保持隐藏（图钉在资源管理页的版本列表里）
        const pinned = result.versions.filter(v => isVersionPinned(v.profileName));
        versionCardList.innerHTML = '';
        if (pinned.length === 0) {
          versionCardList.innerHTML = '<div class="version-list-empty">暂无固定版本<br>到「资源管理 - 版本」用图钉固定</div>';
        }
        pinned.forEach(v => {
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

        // 加载已选版本：卡片底部也只显示固定版本。
        // 选中的那个要是没固定（或根本没选过），就落到第一个固定版本，
        // 免得卡片上显示的和「开始游戏」实际会启动的不是同一个
        tauri.invoke('version.getSelected').then((sel) => {
          const selected = (sel && sel.ok && sel.profileName) ? sel.profileName : '';

          if (selected && isVersionPinned(selected)) {
            currentVersionName = selected;
          } else if (pinned.length > 0) {
            currentVersionName = pinned[0].profileName;
            tauri.invoke('version.select', { profileName: currentVersionName });
          } else {
            currentVersionName = null;
          }

          versionCardName.textContent = currentVersionName || '未固定版本';
          updateVersionListActive();
        }).catch(() => {});
      } else {
        hasInstalledVersions = false;
        pinnedVersionNames = [];
        currentVersionName = null;
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
  // 从版本列表里启动时，那个正在等待结果的行内按钮（见 launchVersionFromList）
  let launchRowBtn = null;

  // 监听游戏启动进度事件
  tauri.listen('game.launch.progress', (event) => {
    console.log('Game launch progress:', event);

    if (event.status === 'waiting_window') {
      startGameBtn.textContent = '等待游戏窗口出现';
    } else if (event.status === 'window_ready') {
      startGameBtn.textContent = '游戏已启动';
      gameLaunchInProgress = false;
      resetLaunchRowBtn();
      setTimeout(() => {
        startGameBtn.textContent = '开始游戏';
        startGameBtn.disabled = false;
      }, 2000);
    } else if (event.status === 'game_ended') {
      startGameBtn.textContent = '开始游戏';
      startGameBtn.disabled = false;
      gameLaunchInProgress = false;
      resetLaunchRowBtn();
    }
  });

  // 启动的最终结果（后端在后台线程里跑完才发）。
  // 失败时后端只发这一条，不会再有 progress 事件——必须在这里把「启动中」收掉，
  // 否则按钮会永远停在启动中（版本文件不完整、Java 未配置等失败都走这条）
  tauri.listen('game.launch.result', (event) => {
    const result = event && event.result;
    if (!result || result.ok) return;   // 成功的收尾交给上面的 game.launch.progress

    gameLaunchInProgress = false;
    startGameBtn.textContent = '开始游戏';
    startGameBtn.disabled = false;
    resetLaunchRowBtn();
    showNotification({
      title: '启动失败',
      content: result.error || '启动游戏失败',
      type: 'error',
      duration: 8000
    });
  });

  // 版本列表里每行的「启动游戏」：直接开这一行的版本。
  // 和主页的「开始游戏」共用 gameLaunchInProgress 这个互斥标记，
  // 收尾（启动完成后恢复按钮）交给上面的 game.launch.progress 事件
  function launchVersionFromList(profileName, btn) {
    if (gameLaunchInProgress) {
      showNotification({
        title: '正在启动中',
        content: '上一次启动还没结束，请稍候再试',
        type: 'info',
        duration: 4000
      });
      return;
    }

    gameLaunchInProgress = true;
    launchRowBtn = btn;
    btn.disabled = true;
    btn.textContent = '启动中...';

    tauri.invoke('game.launch', { profileName: profileName }).then((result) => {
      if (result && result.ok) {
        // 异步启动成功，剩下的交给 game.launch.progress
        btn.textContent = '启动中...';
        return;
      }
      showNotification({
        title: '启动失败',
        content: (result && result.error) || '启动游戏失败',
        type: 'error',
        duration: 5000
      });
      gameLaunchInProgress = false;
      resetLaunchRowBtn();
    }).catch((err) => {
      showNotification({
        title: '启动失败',
        content: (err && err.message) || '启动游戏失败',
        type: 'error',
        duration: 5000
      });
      gameLaunchInProgress = false;
      resetLaunchRowBtn();
    });
  }

  function resetLaunchRowBtn() {
    if (!launchRowBtn) return;
    launchRowBtn.disabled = false;
    launchRowBtn.textContent = '启动游戏';
    launchRowBtn = null;
  }

  // 启动过程中的提醒（目前只有外置登录在 1.19+ 上的聊天签名风险提示）
  tauri.listen('game.launch.notice', (event) => {
    if (!event || !event.title) return;
    showNotification({
      title: event.title,
      content: event.content || '',
      type: event.type || 'info',
      duration: 8000
    });
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
    // 用 transform 而不是 left/top：后者每次 mousemove 都会触发一轮布局，
    // transform 只影响合成层
    tooltip.style.transform = `translate3d(${x}px, ${y}px, 0)`;
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
   * @param {boolean} options.sticky - 常驻卡片：不自动消失，进度条由调用方驱动
   * @returns {number} 通知ID
   */
  function showNotification(options) {
    const {
      title = '通知',
      content = '',
      type = 'info',
      duration = 5000,
      icon = null,
      onClick = null,
      sticky = false,
      // 语义标记。tag=download 的卡片是"进度类"提示，下载任务页正在显示时
      // 会被隐藏（页面本身信息更全），离开任务页再露出来
      tag = ''
    } = options;

    const id = ++notificationId;
    const card = document.createElement('div');
    card.className = 'notification-card';
    card.dataset.id = id;
    if (tag) card.dataset.tag = tag;

    // 卡片内容
    let html = `
      <div class="notification-title">${title}</div>
      <div class="notification-content">${content}</div>
      <button class="notification-close" onclick="dismissNotification(${id})">&times;</button>
    `;

    if (icon) {
      html += `<img class="notification-icon" src="${icon}" alt="icon">`;
    }

    // 进度条。
    // 普通卡片：条就是倒计时，从满缩到空，走完自动消失。
    // sticky 卡片（下载进度这类常驻提示）：条按实际进度从左往右填，
    // 既不启动倒计时，也不挂自动消失的定时器。
    html += sticky
      ? `
      <div class="notification-progress">
        <div class="notification-progress-bar downloading"></div>
      </div>
    `
      : `
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

    if (!sticky) {
      // 启动进度条动画
      setTimeout(() => {
        const progressBar = card.querySelector('.notification-progress-bar');
        if (progressBar) {
          progressBar.style.transform = 'scaleX(0)';
        }
      }, motionDelay(50));

      // 自动消失
      const timer = setTimeout(() => {
        dismissNotification(id);
      }, duration);

      // 存储定时器以便取消
      card.dataset.timer = timer;
    }

    return id;
  }

  /**
   * 就地更新一张已经弹出的卡片（常驻进度卡用）
   * @param {number} id - showNotification 返回的 id
   * @param {Object} options - { title, content, percent }，只更新传了的字段
   * @param {number} options.percent - 0-100，按实际比例填充进度条
   */
  function updateNotification(id, options) {
    const card = notificationContainer.querySelector(`[data-id="${id}"]`);
    if (!card) return;

    const { title, content, percent } = options;
    if (title !== undefined) card.querySelector('.notification-title').textContent = title;
    if (content !== undefined) card.querySelector('.notification-content').textContent = content;

    if (percent !== undefined) {
      const bar = card.querySelector('.notification-progress-bar');
      if (bar) {
        const ratio = Math.max(0, Math.min(100, percent)) / 100;
        bar.style.transform = 'scaleX(' + ratio + ')';
      }
    }
  }

  /**
   * 给常驻卡片收尾：换成结果样式（配色 + 文案），并恢复成会自动消失的普通卡片
   * @param {number} id - showNotification 返回的 id
   * @param {Object} options - { title, content, type, duration }
   */
  function settleNotification(id, options) {
    const card = notificationContainer.querySelector(`[data-id="${id}"]`);
    if (!card) return;

    const { title = '通知', content = '', type = 'info', duration = 5000 } = options;

    // 卡片进入"结果"状态就不再算进度提示了：即便用户正待在下载任务页，
    // 成功/失败的结果也必须看得见
    delete card.dataset.tag;

    card.querySelector('.notification-title').textContent = title;
    card.querySelector('.notification-content').textContent = content;

    const bar = card.querySelector('.notification-progress-bar');
    if (bar) {
      // 从"实际进度"切回"倒计时"：先瞬间填满，再线性缩到 0。
      // 中间必须强制一次重排，否则两个 transform 会被合并成一步、看不到动画。
      bar.className = 'notification-progress-bar ' + type;
      bar.style.transitionDuration = '0ms';
      bar.style.transform = 'scaleX(1)';
      void bar.offsetWidth;
      bar.style.transitionDuration = duration + 'ms';
      bar.style.transform = 'scaleX(0)';
    }

    const timer = setTimeout(() => {
      dismissNotification(id);
    }, duration);
    card.dataset.timer = timer;
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
    }, motionDelay(400));
  }

  /**
   * 清除所有通知
   */
  function clearAllNotifications() {
    notificationContainer.innerHTML = '';
  }

  // ==================== 自绘下拉弹层 ====================
  // 原生 <select> 展开的选项列表是 Chromium 自己的浮层，圆角/底色/阴影都由系统绘制，
  // CSS 改不了。这里保留 <select> 作为数据模型（value / innerHTML / options / change
  // 全部照旧），另外盖一层透明按钮接管点击，弹层换成自己的玻璃面板。
  const DROPDOWN_GAP = 6;
  const DROPDOWN_MAX_HEIGHT = 280;

  let openPanelState = null;

  function closeDropdown() {
    if (!openPanelState) return;
    const { panel, trigger, onPointerDown, onKeyDown, onLayoutChange } = openPanelState;
    openPanelState = null;
    trigger.classList.remove('is-open');
    document.removeEventListener('pointerdown', onPointerDown, true);
    document.removeEventListener('keydown', onKeyDown, true);
    window.removeEventListener('resize', onLayoutChange);
    window.removeEventListener('scroll', onLayoutChange, true);

    // 退场：先挂 is-closing 让它过渡出去，过渡结束再摘节点。
    // 兜底的 setTimeout 是给「过渡没跑起来」的情况收尾（元素不可见 / 过渡被打断），
    // 重复 remove 是无害的，所以两条路径都走到也没关系。
    panel.classList.add('is-closing');
    let removed = false;
    const drop = () => {
      if (removed) return;
      removed = true;
      panel.remove();
    };
    panel.addEventListener('transitionend', drop, { once: true });
    setTimeout(drop, 200);
  }

  function syncDropdownLabel(select) {
    const shell = select.parentElement;
    const trigger = shell ? shell.querySelector('.dropdown-trigger') : null;
    if (!trigger) return;
    const option = select.options[select.selectedIndex];
    trigger.setAttribute('aria-label', option ? option.textContent : '');
  }

  function openDropdown(select, trigger) {
    if (openPanelState && openPanelState.select === select) {
      closeDropdown();
      return;
    }
    closeDropdown();

    const options = Array.from(select.options);
    if (options.length === 0) return;

    const panel = document.createElement('div');
    panel.className = 'dropdown-panel';
    panel.setAttribute('role', 'listbox');

    let activeIndex = Math.max(0, select.selectedIndex);

    const items = options.map((option, index) => {
      const item = document.createElement('div');
      item.className = 'dropdown-option';
      item.setAttribute('role', 'option');
      item.textContent = option.textContent;
      if (index === select.selectedIndex) item.classList.add('is-selected');
      item.addEventListener('click', () => {
        select.value = option.value;
        select.dispatchEvent(new Event('change', { bubbles: true }));
        closeDropdown();
      });
      item.addEventListener('pointerenter', () => setActive(index));
      panel.appendChild(item);
      return item;
    });

    function setActive(index) {
      activeIndex = index;
      items.forEach((item, i) => item.classList.toggle('is-active', i === index));
    }

    document.body.appendChild(panel);
    setActive(activeIndex);

    // 选中项滚到可视区，长列表打开时不会停在顶部
    const current = items[select.selectedIndex];
    if (current) {
      panel.scrollTop = Math.max(0, current.offsetTop - panel.clientHeight / 2 + current.offsetHeight / 2);
    }

    // 面板按最长选项自然撑开但不超过视口；比触发按钮窄时至少和按钮等宽
    const rect = trigger.getBoundingClientRect();
    panel.style.maxWidth = (window.innerWidth - DROPDOWN_GAP * 2) + 'px';
    const panelWidth = Math.min(
      window.innerWidth - DROPDOWN_GAP * 2,
      Math.max(rect.width, panel.offsetWidth)
    );
    panel.style.width = panelWidth + 'px';
    panel.style.left = Math.max(
      DROPDOWN_GAP,
      Math.min(rect.left, window.innerWidth - panelWidth - DROPDOWN_GAP)
    ) + 'px';

    // 下方放不下就翻到上方
    const panelHeight = Math.min(panel.offsetHeight, DROPDOWN_MAX_HEIGHT);
    const spaceBelow = window.innerHeight - rect.bottom - DROPDOWN_GAP;
    panel.style.top = (spaceBelow >= panelHeight
      ? rect.bottom + DROPDOWN_GAP
      : Math.max(DROPDOWN_GAP, rect.top - DROPDOWN_GAP - panelHeight)) + 'px';

    const onPointerDown = (event) => {
      if (panel.contains(event.target) || trigger.contains(event.target)) return;
      closeDropdown();
    };

    const onKeyDown = (event) => {
      if (event.key === 'Escape') {
        event.preventDefault();
        trigger.focus();
        closeDropdown();
      } else if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
        event.preventDefault();
        const step = event.key === 'ArrowDown' ? 1 : -1;
        setActive(Math.min(items.length - 1, Math.max(0, activeIndex + step)));
        items[activeIndex].scrollIntoView({ block: 'nearest' });
      } else if (event.key === 'Enter' || event.key === ' ') {
        event.preventDefault();
        const option = options[activeIndex];
        if (option) {
          select.value = option.value;
          select.dispatchEvent(new Event('change', { bubbles: true }));
        }
        closeDropdown();
      }
    };

    // 页面滚动 / 缩放时面板会跟触发按钮错位，直接关掉。
    // 但要放过面板自身的滚动：scroll 不冒泡、capture 却会从 window 传下来，
    // 不过滤的话滚轮和拖动滚动条都会立刻把面板关掉。
    const onLayoutChange = (event) => {
      if (event && event.target instanceof Node && panel.contains(event.target)) return;
      closeDropdown();
    };

    document.addEventListener('pointerdown', onPointerDown, true);
    document.addEventListener('keydown', onKeyDown, true);
    window.addEventListener('resize', onLayoutChange);
    window.addEventListener('scroll', onLayoutChange, true);

    trigger.classList.add('is-open');
    openPanelState = { select, panel, trigger, onPointerDown, onKeyDown, onLayoutChange };
  }

  function enhanceSelect(select) {
    if (select.dataset.glassDropdown === '1' || !select.parentNode) return;
    select.dataset.glassDropdown = '1';

    const shell = document.createElement('span');
    shell.className = 'dropdown';
    select.parentNode.insertBefore(shell, select);
    shell.appendChild(select);

    const trigger = document.createElement('button');
    trigger.type = 'button';
    trigger.className = 'dropdown-trigger';
    trigger.setAttribute('aria-haspopup', 'listbox');
    trigger.addEventListener('click', () => {
      if (select.disabled) return;
      openDropdown(select, trigger);
    });
    shell.appendChild(trigger);

    // select 已被透明按钮盖住，从 Tab 顺序里摘掉，避免聚焦后方向键又弹出原生列表
    select.tabIndex = -1;
    select.addEventListener('change', () => syncDropdownLabel(select));
    syncDropdownLabel(select);
  }

  function enhanceSelects(root) {
    if (root instanceof HTMLSelectElement) enhanceSelect(root);
    const scope = root instanceof Element ? root : document;
    scope.querySelectorAll('select').forEach(enhanceSelect);
  }

  enhanceSelects(document);
  new MutationObserver((records) => {
    records.forEach((record) => {
      record.addedNodes.forEach((node) => {
        if (node.nodeType === 1) enhanceSelects(node);
      });
    });
  }).observe(document.body, { childList: true, subtree: true });

  // ==================== 联机（内网穿透）页 ====================
  // 映射令牌由用户从 SakuraFrp 个人中心自行获取（后端校验后加密落盘）；
  // 隧道由同目录的 frpc 二进制在本地拉起，后端只负责配置、API 与进程托管。
  const onlineContent = document.getElementById('onlineContent');
  const onlineSubSidebarItems = document.querySelectorAll('#onlineSubSidebar .sub-sidebar-item');
  let activeOnlineCategory = 'control';

  const ONLINE_PROFILE_URL = 'https://www.natfrp.com/user/profile';
  const ONLINE_SITE_URL = 'https://www.natfrp.com/';
  const ONLINE_DEFAULT_PORT = 25565;
  // 按钮 / 输入框的外观全部走样式表（.online-btn / .online-input）：
  // 内联 cssText 改不了伪类，hover / active / focus 的过渡会整体缺失
  const ONLINE_ROW = 'display:flex;align-items:center;gap:8px;flex-wrap:wrap;';

  // 页面代际：await 回来时用户可能已经切走，旧响应不能再往 DOM 上写
  let onlineViewSeq = 0;
  // 隧道远端地址要靠节点 host 才能拼出来，两者一起缓存
  let onlineTunnelsCache = [];
  let onlineNodesCache = [];
  let onlineRunning = false;
  let onlineLogs = [];

  function onlineSection(title) {
    const wrapper = document.createElement('div');
    wrapper.className = 'settings-section';
    const head = document.createElement('div');
    head.className = 'settings-section-title';
    head.textContent = title;
    wrapper.appendChild(head);
    return wrapper;
  }

  function onlineHint(text) {
    const el = document.createElement('div');
    el.className = 'online-hint';
    el.textContent = text;
    return el;
  }

  // variant: primary | danger | ghost
  function onlineButton(text, variant) {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'online-btn is-' + variant;
    btn.textContent = text;
    return btn;
  }

  function onlineRow(label, value) {
    const row = document.createElement('div');
    row.className = 'settings-item';
    row.style.cursor = 'default';
    row.innerHTML = `<span class="settings-item-label">${label}</span><span class="settings-item-value">${value}</span>`;
    return row;
  }

  function onlineLoading(text) {
    onlineContent.innerHTML = '';
    const box = document.createElement('div');
    box.className = 'download-loading';
    box.innerHTML = `<div class="download-spinner"></div><p>${text}</p>`;
    onlineContent.appendChild(box);
  }

  function onlineEmpty(text) {
    const box = document.createElement('div');
    box.className = 'online-empty';
    box.textContent = text;
    return box;
  }

  function onlineNodeTags(flag) {
    const value = Number(flag) || 0;
    const tags = [];
    if (value & (1 << 9)) tags.push('离线');
    else tags.push(value & (1 << 3) ? '内地' : '海外');
    if (value & (1 << 5)) tags.push('UDP');
    return tags;
  }

  // tcp 隧道连接地址 = 节点 host : 远端端口；http(s) 隧道直接用绑定域名
  function onlineTunnelAddress(tunnelId) {
    const tunnel = onlineTunnelsCache.find(t => String(t.id) === String(tunnelId));
    if (!tunnel) return '';
    if (tunnel.type === 'http' || tunnel.type === 'https') return String(tunnel.remote || '');
    const node = onlineNodesCache.find(n => String(n.id) === String(tunnel.node));
    const host = node ? String(node.host || '') : '';
    const port = String(tunnel.remote || '');
    if (host && port) return host + ':' + port;
    return host || port;
  }

  function appendOnlineLog(line) {
    onlineLogs.push(line);
    while (onlineLogs.length > 400) onlineLogs.shift();
    const panel = document.getElementById('onlineLogPanel');
    if (!panel) return;
    const row = document.createElement('div');
    row.className = 'online-log-line';
    row.textContent = line;
    panel.appendChild(row);
    while (panel.childNodes.length > 400) panel.removeChild(panel.firstChild);
    panel.scrollTop = panel.scrollHeight;
  }

  function renderOnlineLogPanel(container) {
    const panel = document.createElement('div');
    panel.className = 'online-log';
    panel.id = 'onlineLogPanel';
    onlineLogs.forEach(line => {
      const row = document.createElement('div');
      row.className = 'online-log-line';
      row.textContent = line;
      panel.appendChild(row);
    });
    container.appendChild(panel);
    panel.scrollTop = panel.scrollHeight;
  }

  // 被 state 事件触发时就地刷新按钮/状态，不必整页重绘
  function syncOnlineControlUI() {
    const btn = document.getElementById('onlineToggleBtn');
    const pill = document.getElementById('onlineStatusPill');
    if (pill) {
      pill.textContent = onlineRunning ? '映射运行中' : '未运行';
      pill.classList.toggle('is-on', onlineRunning);
    }
    if (btn) {
      btn.textContent = onlineRunning ? '停止映射' : '开始映射';
      btn.classList.toggle('is-danger', onlineRunning);
      btn.classList.toggle('is-primary', !onlineRunning);
    }
  }

  function openNatfrpUrl(url) {
    tauri.invoke('shell.open', { url });
  }

  async function renderOnlineControl() {
    const view = ++onlineViewSeq;
    onlineLoading('正在读取联机配置');

    const state = await tauri.invoke('multiplayer.state').catch(() => null);
    if (view !== onlineViewSeq) return;
    if (!state || !state.ok) {
      onlineContent.innerHTML = '';
      onlineContent.appendChild(onlineEmpty('无法读取联机配置'));
      return;
    }

    onlineRunning = !!state.running;
    onlineLogs = Array.isArray(state.logs) ? state.logs.slice() : [];
    onlineTunnelsCache = [];
    onlineNodesCache = [];

    let user = null;
    if (state.hasToken) {
      const [tunnelsRes, nodesRes, userRes] = await Promise.all([
        tauri.invoke('multiplayer.tunnels').catch(() => null),
        tauri.invoke('multiplayer.nodes').catch(() => null),
        tauri.invoke('multiplayer.user').catch(() => null)
      ]);
      if (view !== onlineViewSeq) return;
      if (tunnelsRes && tunnelsRes.ok && Array.isArray(tunnelsRes.tunnels)) onlineTunnelsCache = tunnelsRes.tunnels;
      if (nodesRes && nodesRes.ok && Array.isArray(nodesRes.nodes)) onlineNodesCache = nodesRes.nodes;
      if (userRes && userRes.ok) user = userRes.user;
    }

    onlineContent.innerHTML = '';

    // ---- 映射令牌 ----
    const tokenSection = onlineSection('映射令牌');
    const accountName = user ? (user.username || user.email || ('UID ' + (user.id || ''))) : '';
    tokenSection.appendChild(onlineRow(
      '当前状态',
      state.hasToken
        ? `已设置（尾号 ${state.tokenTail || '----'}）${accountName ? ' · ' + accountName : ''}`
        : '未设置'
    ));

    const tokenRow = document.createElement('div');
    tokenRow.className = 'settings-item';
    tokenRow.style.cssText = ONLINE_ROW + 'cursor:default;';
    const tokenInput = document.createElement('input');
    tokenInput.type = 'text';
    tokenInput.placeholder = state.hasToken ? '粘贴新令牌以替换' : '粘贴访问密钥';
    tokenInput.className = 'online-input';
    const tokenSave = onlineButton('保存', 'primary');
    tokenRow.appendChild(tokenInput);
    tokenRow.appendChild(tokenSave);
    if (state.hasToken) {
      const tokenClear = onlineButton('清除', 'danger');
      tokenClear.addEventListener('click', () => {
        tauri.invoke('multiplayer.clearToken').then(() => {
          showNotification({ title: '联机', content: '已清除映射令牌', type: 'success' });
          renderOnlineControl();
        });
      });
      tokenRow.appendChild(tokenClear);
    }
    tokenSection.appendChild(tokenRow);
    tokenSave.addEventListener('click', () => {
      const token = tokenInput.value.trim();
      if (!token) return;
      tokenSave.disabled = true;
      tauri.invoke('multiplayer.saveToken', { token }).then((res) => {
        tokenSave.disabled = false;
        if (!res || !res.ok) {
          showNotification({ title: '令牌校验失败', content: (res && res.error) || '请检查令牌是否正确', type: 'error' });
          return;
        }
        tokenInput.value = '';
        showNotification({ title: '联机', content: '令牌已保存', type: 'success' });
        renderOnlineControl();
      });
    });
    const profileBtn = onlineButton('前往个人中心获取令牌', 'ghost');
    profileBtn.addEventListener('click', () => openNatfrpUrl(ONLINE_PROFILE_URL));
    tokenSection.appendChild(profileBtn);
    tokenSection.appendChild(onlineHint(
      '访问密钥即映射令牌，在 SakuraFrp 个人中心「访问密钥」一栏获取；它只保存在本机（DPAPI 加密），不会显示在界面上。'));
    onlineContent.appendChild(tokenSection);

    // ---- 本地映射 ----
    const mapSection = onlineSection('本地映射');
    if (!state.hasToken) {
      mapSection.appendChild(onlineHint('保存令牌后即可选择隧道并开启映射。'));
    } else {
      const portRow = document.createElement('div');
      portRow.className = 'settings-item';
      portRow.style.cssText = ONLINE_ROW + 'cursor:default;';
      portRow.innerHTML = '<span class="settings-item-label">本地端口</span>';
      const portInput = document.createElement('input');
      portInput.type = 'number';
      portInput.min = '1';
      portInput.max = '65535';
      portInput.value = String(state.localPort || ONLINE_DEFAULT_PORT);
      portInput.className = 'online-input is-port';
      portRow.appendChild(portInput);
      mapSection.appendChild(portRow);
      mapSection.appendChild(onlineHint('游戏「对局域网开放」后显示的端口，或你自建服务器的端口（默认 25565）。'));

      const tunnelRow = document.createElement('div');
      tunnelRow.className = 'settings-item';
      tunnelRow.style.cssText = ONLINE_ROW + 'cursor:default;';
      tunnelRow.innerHTML = '<span class="settings-item-label">映射隧道</span>';
      const tunnelSelect = document.createElement('select');
      tunnelSelect.style.flex = '1';
      if (onlineTunnelsCache.length === 0) {
        const opt = document.createElement('option');
        opt.value = '';
        opt.textContent = '暂无隧道，请先到「隧道」页新建';
        tunnelSelect.appendChild(opt);
      } else {
        onlineTunnelsCache.forEach(t => {
          const opt = document.createElement('option');
          opt.value = String(t.id);
          opt.textContent = `#${t.id} ${t.name || ''} (${t.type || 'tcp'})`;
          if (String(t.id) === String(state.tunnelId)) opt.selected = true;
          tunnelSelect.appendChild(opt);
        });
      }
      tunnelRow.appendChild(tunnelSelect);
      mapSection.appendChild(tunnelRow);

      const actionRow = document.createElement('div');
      actionRow.className = 'settings-item';
      actionRow.style.cssText = ONLINE_ROW + 'cursor:default;justify-content:flex-start;';
      const toggleBtn = onlineButton(onlineRunning ? '停止映射' : '开始映射', onlineRunning ? 'danger' : 'primary');
      toggleBtn.id = 'onlineToggleBtn';
      if (!onlineRunning && onlineTunnelsCache.length === 0) toggleBtn.disabled = true;
      const pill = document.createElement('span');
      pill.id = 'onlineStatusPill';
      pill.className = 'online-status-pill' + (onlineRunning ? ' is-on' : '');
      pill.textContent = onlineRunning ? '映射运行中' : '未运行';
      actionRow.appendChild(toggleBtn);
      actionRow.appendChild(pill);
      mapSection.appendChild(actionRow);

      if (!state.frpcFound) {
        mapSection.appendChild(onlineHint('未找到 frpc 程序，无法开启映射。请确认 frpc.exe 与启动器在同一目录。'));
      }
      if (onlineTunnelsCache.length === 0) {
        mapSection.appendChild(onlineHint('当前账号还没有隧道，去「隧道」页新建一条即可。'));
      }

      const persist = () => {
        tauri.invoke('multiplayer.select', {
          tunnelId: parseInt(tunnelSelect.value, 10) || 0,
          localPort: parseInt(portInput.value, 10) || ONLINE_DEFAULT_PORT
        });
        updateOnlineAddress();
      };
      // 声明式提升：persist / 地址区都能引用到，切换隧道时地址即时刷新
      function updateOnlineAddress() {
        const el = document.getElementById('onlineAddressValue');
        if (el) el.textContent = onlineTunnelAddress(tunnelSelect.value) || '选择隧道后显示';
      }
      tunnelSelect.addEventListener('change', persist);
      portInput.addEventListener('change', persist);

      toggleBtn.addEventListener('click', () => {
        toggleBtn.disabled = true;
        const tunnelId = parseInt(tunnelSelect.value, 10) || 0;
        const localPort = parseInt(portInput.value, 10) || 0;
        const call = onlineRunning
          ? tauri.invoke('multiplayer.stop')
          : tauri.invoke('multiplayer.start', { tunnelId, localPort });
        call.then((res) => {
          toggleBtn.disabled = false;
          if (!res || !res.ok) {
            showNotification({ title: '联机', content: (res && res.error) || '操作失败', type: 'error' });
            return;
          }
          renderOnlineControl();
        }).catch(() => {
          toggleBtn.disabled = false;
          showNotification({ title: '联机', content: '操作失败', type: 'error' });
        });
      });

      // ---- 连接地址 ----
      const addrSection = onlineSection('连接地址');
      const address = onlineTunnelAddress(state.tunnelId || tunnelSelect.value);
      const addrRow = document.createElement('div');
      addrRow.className = 'settings-item';
      addrRow.style.cssText = ONLINE_ROW + 'cursor:default;';
      addrRow.innerHTML = `<span class="settings-item-label">服务器地址</span><span class="settings-item-value" id="onlineAddressValue" style="flex:1;word-break:break-all;">${address || '选择隧道后显示'}</span>`;
      if (address) {
        const copyBtn = onlineButton('复制', 'ghost');
        copyBtn.addEventListener('click', () => {
          const text = document.getElementById('onlineAddressValue');
          tauri.invoke('clipboard.write', { text: text ? text.textContent : '' }).then(() => {
            showNotification({ title: '联机', content: '已复制连接地址', type: 'success' });
          });
        });
        addrRow.appendChild(copyBtn);
      }
      addrSection.appendChild(addrRow);
      addrSection.appendChild(onlineHint('把该地址发给要联机的朋友，在「多人游戏 → 直接连接」里输入即可。'));
      onlineContent.appendChild(mapSection);
      onlineContent.appendChild(addrSection);

      // ---- 运行日志 ----
      const logSection = onlineSection('运行日志');
      renderOnlineLogPanel(logSection);
      onlineContent.appendChild(logSection);

      syncOnlineControlUI();
      return;
    }

    onlineContent.appendChild(mapSection);
  }

  async function renderOnlineTunnels() {
    const view = ++onlineViewSeq;
    onlineLoading('正在读取隧道列表');

    const state = await tauri.invoke('multiplayer.state').catch(() => null);
    if (view !== onlineViewSeq) return;
    if (!state || !state.ok || !state.hasToken) {
      onlineContent.innerHTML = '';
      onlineContent.appendChild(onlineEmpty('请先在「联机」页设置映射令牌'));
      return;
    }

    const [tunnelsRes, nodesRes] = await Promise.all([
      tauri.invoke('multiplayer.tunnels').catch(() => null),
      tauri.invoke('multiplayer.nodes').catch(() => null)
    ]);
    if (view !== onlineViewSeq) return;

    onlineTunnelsCache = (tunnelsRes && tunnelsRes.ok && Array.isArray(tunnelsRes.tunnels)) ? tunnelsRes.tunnels : [];
    onlineNodesCache = (nodesRes && nodesRes.ok && Array.isArray(nodesRes.nodes)) ? nodesRes.nodes : [];

    onlineContent.innerHTML = '';

    // ---- 新建隧道 ----
    const createSection = onlineSection('新建隧道');
    const nameRow = document.createElement('div');
    nameRow.className = 'settings-item';
    nameRow.style.cssText = ONLINE_ROW + 'cursor:default;';
    nameRow.innerHTML = '<span class="settings-item-label">隧道名称</span>';
    const nameInput = document.createElement('input');
    nameInput.type = 'text';
    nameInput.placeholder = '例如 Minecraft 联机';
    nameInput.className = 'online-input';
    nameRow.appendChild(nameInput);
    createSection.appendChild(nameRow);

    const nodeRow = document.createElement('div');
    nodeRow.className = 'settings-item';
    nodeRow.style.cssText = ONLINE_ROW + 'cursor:default;';
    nodeRow.innerHTML = '<span class="settings-item-label">节点</span>';
    const nodeSelect = document.createElement('select');
    nodeSelect.style.flex = '1';
    const onlineNodes = onlineNodesCache.filter(n => !(Number(n.flag) & (1 << 9)));
    (onlineNodes.length ? onlineNodes : onlineNodesCache).forEach(n => {
      const opt = document.createElement('option');
      opt.value = String(n.id);
      opt.textContent = `${n.name || ('节点 ' + n.id)} · ${n.host || ''}`;
      nodeSelect.appendChild(opt);
    });
    if (!onlineNodesCache.length) {
      const opt = document.createElement('option');
      opt.value = '';
      opt.textContent = '暂无可用节点';
      nodeSelect.appendChild(opt);
    }
    nodeRow.appendChild(nodeSelect);
    createSection.appendChild(nodeRow);

    const createPortRow = document.createElement('div');
    createPortRow.className = 'settings-item';
    createPortRow.style.cssText = ONLINE_ROW + 'cursor:default;';
    createPortRow.innerHTML = '<span class="settings-item-label">本地端口</span>';
    const createPortInput = document.createElement('input');
    createPortInput.type = 'number';
    createPortInput.min = '1';
    createPortInput.max = '65535';
    createPortInput.value = String(state.localPort || ONLINE_DEFAULT_PORT);
    createPortInput.className = 'online-input is-port';
    createPortRow.appendChild(createPortInput);
    createSection.appendChild(createPortRow);

    const createBtn = onlineButton('创建隧道', 'primary');
    createSection.appendChild(createBtn);
    createSection.appendChild(onlineHint('隧道类型固定为 TCP（适配 Minecraft 联机），创建后可在「联机」页选择并开启映射。'));
    createBtn.addEventListener('click', () => {
      const name = nameInput.value.trim();
      if (!name) {
        showNotification({ title: '联机', content: '请填写隧道名称', type: 'error' });
        return;
      }
      createBtn.disabled = true;
      tauri.invoke('multiplayer.createTunnel', {
        name,
        node: parseInt(nodeSelect.value, 10) || 0,
        localPort: parseInt(createPortInput.value, 10) || ONLINE_DEFAULT_PORT
      }).then((res) => {
        createBtn.disabled = false;
        if (!res || !res.ok) {
          showNotification({ title: '创建失败', content: (res && res.error) || '请稍后重试', type: 'error' });
          return;
        }
        showNotification({ title: '联机', content: '隧道创建成功', type: 'success' });
        renderOnlineTunnels();
      }).catch(() => {
        createBtn.disabled = false;
        showNotification({ title: '创建失败', content: '请稍后重试', type: 'error' });
      });
    });
    onlineContent.appendChild(createSection);

    // ---- 隧道列表 ----
    const listSection = onlineSection('隧道列表');
    if (onlineTunnelsCache.length === 0) {
      listSection.appendChild(onlineEmpty('暂无隧道'));
    } else {
      onlineTunnelsCache.forEach(t => {
        const row = document.createElement('div');
        row.className = 'settings-item';
        row.style.cursor = 'default';
        const node = onlineNodesCache.find(n => String(n.id) === String(t.node));
        const remote = String(t.remote || '');
        // 元信息拆两行：一行最多一个分隔符，堆成「a · b · c · d」很难扫读
        row.innerHTML = `
          <span class="online-row-info">
            <span class="settings-item-label">#${t.id} ${t.name || ''}</span>
            <span class="online-row-meta">${t.type || 'tcp'} · 节点 ${node ? (node.name || t.node) : t.node}</span>
            <span class="online-row-meta">本地 ${t.local_port || '?'} → 远端 ${remote || '未分配'}</span>
          </span>
        `;
        const actions = document.createElement('span');
        actions.style.cssText = 'display:flex;gap:6px;flex-shrink:0;';
        const useBtn = onlineButton('使用', 'ghost');
        useBtn.addEventListener('click', () => {
          tauri.invoke('multiplayer.select', { tunnelId: t.id }).then(() => {
            activeOnlineCategory = 'control';
            onlineSubSidebarItems.forEach(i => i.classList.toggle('active', i.dataset.category === 'control'));
            sidebarIcons.forEach(i => i.classList.toggle('active', i.dataset.tab === 'online'));
            // 本来就在联机页时 switchPage 会提前返回，直接切子视图即可
            if (onlinePage.classList.contains('active')) loadOnlineContent('control');
            else switchPage('online');
          });
        });
        const delBtn = onlineButton('删除', 'danger');
        delBtn.addEventListener('click', () => {
          delBtn.disabled = true;
          tauri.invoke('multiplayer.deleteTunnel', { ids: String(t.id) }).then((res) => {
            delBtn.disabled = false;
            if (!res || !res.ok) {
              showNotification({ title: '删除失败', content: (res && res.error) || '请稍后重试', type: 'error' });
              return;
            }
            showNotification({ title: '联机', content: '隧道已删除', type: 'success' });
            renderOnlineTunnels();
          });
        });
        actions.appendChild(useBtn);
        actions.appendChild(delBtn);
        row.appendChild(actions);
        listSection.appendChild(row);
      });
    }
    onlineContent.appendChild(listSection);
  }

  async function renderOnlineNodes() {
    const view = ++onlineViewSeq;
    onlineLoading('正在读取节点列表');

    const res = await tauri.invoke('multiplayer.nodes').catch(() => null);
    if (view !== onlineViewSeq) return;
    if (!res || !res.ok) {
      onlineContent.innerHTML = '';
      onlineContent.appendChild(onlineEmpty((res && res.error) || '请先在「联机」页设置映射令牌'));
      return;
    }

    const nodes = Array.isArray(res.nodes) ? res.nodes : [];
    onlineNodesCache = nodes;

    onlineContent.innerHTML = '';
    const section = onlineSection('节点列表');
    if (nodes.length === 0) {
      section.appendChild(onlineEmpty('暂无节点'));
    } else {
      nodes.forEach(n => {
        const row = document.createElement('div');
        row.className = 'settings-item';
        row.style.cursor = 'default';
        const offline = Number(n.flag) & (1 << 9);
        const tags = onlineNodeTags(n.flag).map(tag =>
          `<span class="online-tag${tag === '离线' ? ' is-off' : ''}">${tag}</span>`).join('');
        row.innerHTML = `
          <span class="online-row-info">
            <span class="settings-item-label">${n.name || ('节点 ' + n.id)}</span>
            <span class="online-row-meta">${n.host || ''}</span>
          </span>
          <span class="online-row-side">${tags}
            <span class="online-row-state${offline ? ' is-off' : ''}">${offline ? '离线' : '在线'}</span>
          </span>
        `;
        section.appendChild(row);
      });
    }
    onlineContent.appendChild(section);
  }

  function loadOnlineContent(category) {
    if (!onlineContent) return;
    if (category === 'tunnels') renderOnlineTunnels();
    else if (category === 'nodes') renderOnlineNodes();
    else renderOnlineControl();
  }

  onlineSubSidebarItems.forEach(item => {
    item.addEventListener('click', () => {
      onlineSubSidebarItems.forEach(i => i.classList.remove('active'));
      item.classList.add('active');
      activeOnlineCategory = item.dataset.category;
      loadOnlineContent(activeOnlineCategory);
    });
  });

  // 底部归属提示：常驻元素，点一下去樱花映射官网
  const onlineAttribution = document.getElementById('onlineAttribution');
  if (onlineAttribution) {
    onlineAttribution.addEventListener('click', () => openNatfrpUrl(ONLINE_SITE_URL));
  }

  // 后端推来的 frpc 日志与进程状态：只增量更新当前视图
  tauri.listen('multiplayer.log', (event) => {
    if (event && typeof event.line === 'string') appendOnlineLog(event.line);
  });
  tauri.listen('multiplayer.state', (event) => {
    if (!event) return;
    onlineRunning = !!event.running;
    syncOnlineControlUI();
  });

  // ==================== 整合包拖放安装 ====================
  // 拖放本身由 C++ 原生层接管（WebView2 不给页面文件路径），这里只负责
  // 收事件 + 出确认卡片 + 显示进度。识别和安装全在后端，前端不碰文件系统。
  const dropOverlay = document.getElementById('dropOverlay');
  const modpackMask = document.getElementById('modpackMask');
  const modpackDialog = document.getElementById('modpackDialog');

  let modpackBusy = false;
  let modpackNoticeId = null;
  let modpackPolling = false;
  let modpackTaskId = null;

  function formatBytes(bytes) {
    if (!bytes || bytes <= 0) return '未知';
    const units = ['B', 'KB', 'MB', 'GB'];
    let value = bytes;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
      value /= 1024;
      unit++;
    }
    return (unit === 0 ? value.toFixed(0) : value.toFixed(1)) + ' ' + units[unit];
  }

  function closeModpackDialog() {
    if (modpackMask) modpackMask.hidden = true;
    if (modpackDialog) modpackDialog.innerHTML = '';
  }

  // 整合包里的包名、加载器名、警告文案全部来自压缩包内的 manifest.json，
  // 是外部可控数据。这张页面手里有 __tauricpp__ 桥（等于能调任意后端命令），
  // 直接拼进 innerHTML 就是现成的 XSS 入口，所以一律先转义
  function escapeHtml(value) {
    return String(value === null || value === undefined ? '' : value)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  function modpackRow(label, value) {
    return `<div class="modpack-row">
      <span class="modpack-row-label">${escapeHtml(label)}</span>
      <span class="modpack-row-value">${escapeHtml(value)}</span>
    </div>`;
  }

  function openModpackDialog(info) {
    if (!modpackDialog || !modpackMask) return;

    const isModrinth = info.type === 'modrinth';
    const loaderText = info.loaderType
      ? `${info.loaderType}${info.loaderVersion ? ' ' + info.loaderVersion : ''}`
      : '无（纯原版）';

    let html = `
      <div class="modpack-dialog-title">
        <span>安装整合包</span>
        <span class="modpack-badge ${isModrinth ? '' : 'is-cf'}">${isModrinth ? 'Modrinth' : 'CurseForge'}</span>
      </div>
      <div class="modpack-name">${escapeHtml(info.packName || '（整合包未命名）')}</div>
      <div class="modpack-rows">
        ${modpackRow('Minecraft 版本', info.gameVersion || '未知')}
        ${modpackRow('加载器', loaderText)}
        ${modpackRow('待下载文件', `${info.fileCount} 个`)}
        ${modpackRow('下载体积', formatBytes(info.totalSize))}
      </div>`;

    if (info.warnings && info.warnings.length) {
      html += `<div class="modpack-warnings">${info.warnings
        .map((w) => `<div>· ${escapeHtml(w)}</div>`)
        .join('')}</div>`;
    }

    html += `
      <div class="modpack-field">
        <div class="modpack-field-label">实例名</div>
        <input class="download-info-input" id="modpackNameInput" value="${escapeHtml(info.suggestedName || '')}" spellcheck="false">
      </div>`;

    if (info.optionalCount > 0) {
      html += `
        <label class="modpack-checkbox">
          <input type="checkbox" id="modpackOptional">
          下载 ${info.optionalCount} 个可选文件
        </label>`;
    }

    html += `
      <div class="modpack-actions">
        <button class="modpack-btn is-secondary" id="modpackCancelBtn">取消</button>
        <button class="modpack-btn is-primary" id="modpackInstallBtn">开始安装</button>
      </div>`;

    modpackDialog.innerHTML = html;
    modpackMask.hidden = false;

    const nameInput = document.getElementById('modpackNameInput');
    if (nameInput) {
      nameInput.focus();
      nameInput.select();
      nameInput.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') startModpackInstall(info);
      });
    }
    document.getElementById('modpackCancelBtn').addEventListener('click', closeModpackDialog);
    document.getElementById('modpackInstallBtn').addEventListener('click', () => startModpackInstall(info));
  }

  async function startModpackInstall(info) {
    if (modpackBusy) return;

    const nameInput = document.getElementById('modpackNameInput');
    const instanceName = (nameInput && nameInput.value.trim()) || info.suggestedName || '';
    if (!instanceName) return;

    const optionalBox = document.getElementById('modpackOptional');
    const downloadOptional = !!(optionalBox && optionalBox.checked);

    closeModpackDialog();
    modpackBusy = true;

    let resp = null;
    try {
      resp = await tauri.invoke('modpack.install', {
        path: info.path,
        instanceName: instanceName,
        downloadOptional: downloadOptional
      });
    } catch (e) {
      resp = { ok: false, error: String(e) };
    }
    if (!resp || !resp.ok) {
      modpackBusy = false;
      showNotification({
        title: '无法开始安装',
        content: (resp && resp.error) || '未知错误',
        type: 'error',
        duration: 8000
      });
      return;
    }

    modpackTaskId = createDownloadTask({
      kind: 'modpack',
      title: (info.packName || instanceName) + '（整合包）',
      subtitle: '正在准备…',
      totalBytes: info.totalSize || 0,
      cancelCmd: 'modpack.cancel'
    });
    // 任何下载任务一开始就把用户带到任务页，光靠一个小通知确认感太弱
    openTasksPage();

    modpackNoticeId = showNotification({
      title: '安装整合包',
      content: '正在准备…（按 Esc 可取消）',
      type: 'info',
      sticky: true,
      tag: 'download',
      onClick: openTasksPage
    });
    pollModpackInstall();
  }

  function pollModpackInstall() {
    if (modpackPolling) return;
    modpackPolling = true;

    const tick = async () => {
      let resp = null;
      try {
        resp = await tauri.invoke('modpack.poll');
      } catch (e) {
        resp = null;
      }
      if (!resp) {
        setTimeout(tick, 1000);
        return;
      }

      if (resp.progress) {
        const progress = resp.progress;
        // 进度条的百分比有两个来源：modpack.progress（阶段百分比）和
        // download.progress（装原版阶段的字节比例）。两者会互相覆盖，
        // 所以统一取历史最大值 —— 安装流程的进度本来就是单调的，
        // 取 max 既不会丢信息，也彻底消掉来回横跳
        const task = modpackTaskId === null ? null : downloadTasks.get(modpackTaskId);
        const rawPercent = typeof progress.percent === 'number' ? progress.percent : 0;
        const percent = task ? Math.max(task.percent || 0, rawPercent) : rawPercent;

        if (modpackNoticeId !== null) {
          updateNotification(modpackNoticeId, {
            content: progress.message || '正在安装…',
            percent: percent
          });
        }
        updateDownloadTask(modpackTaskId, {
          subtitle: progress.message || '正在安装…',
          phase: progress.phase || '',
          percent: percent,
          completedFiles: progress.completedFiles || 0,
          totalFiles: progress.totalFiles || 0,
          // totalBytes 只在下载阶段有值，前面几个阶段不动它，避免剩余大小忽有忽无
          ...(progress.totalBytes ? { totalBytes: progress.totalBytes } : {}),
          ...(typeof progress.downloadedBytes === 'number'
            ? { downloadedBytes: progress.downloadedBytes }
            : {})
        });
      }

      if (resp.hasResult) {
        modpackPolling = false;
        modpackBusy = false;
        const result = resp.result || {};
        if (result.ok) {
          finishDownloadTask(modpackTaskId, 'done', '');
        } else if ((result.error || '').includes('取消')) {
          finishDownloadTask(modpackTaskId, 'cancelled', '已取消');
        } else {
          finishDownloadTask(modpackTaskId, 'failed', result.error || '未知错误');
        }
        modpackTaskId = null;
        if (modpackNoticeId !== null) {
          if (result.ok) {
            settleNotification(modpackNoticeId, {
              title: '整合包安装完成',
              content: `${result.instanceName}　Minecraft ${result.gameVersion || ''}`.trim(),
              type: 'success',
              duration: 9000
            });
          } else {
            settleNotification(modpackNoticeId, {
              title: '整合包安装失败',
              content: result.error || '未知错误',
              type: 'error',
              duration: 12000
            });
          }
        }
        modpackNoticeId = null;

        if (result.ok) {
          // 新版本已经写进 versions\ 目录，刷新主页版本卡和资源页的版本列表
          if (typeof loadVersionCard === 'function') loadVersionCard();
          if (typeof loadResourceVersions === 'function') loadResourceVersions();
        }
        return;
      }

      setTimeout(tick, 500);
    };

    setTimeout(tick, 300);
  }

  async function handleModpackDrop(path) {
    if (modpackBusy) {
      showNotification({
        title: '请稍候',
        content: '已有整合包正在安装中',
        type: 'warn',
        duration: 4000
      });
      return;
    }

    if (/\.rar$/i.test(path)) {
      showNotification({
        title: '不支持 rar 格式',
        content: '请先解压，再重新打包成 zip 后拖入',
        type: 'error',
        duration: 8000
      });
      return;
    }

    // 松手后立刻给反馈。识别 CurseForge 整合包还要额外请求一次接口，
    // 快则一两秒、慢则好几秒，这段时间什么都不显示的话看起来就是"没反应"
    const loadingId = showNotification({
      title: '正在读取整合包',
      content: fileNameOf(path),
      type: 'info',
      sticky: true
    });

    let info = null;
    try {
      info = await withTimeout(
        tauri.invoke('modpack.inspect', { path: path }),
        30000,
        '读取整合包超时'
      );
    } catch (e) {
      info = { ok: false, error: (e && e.message) || String(e) };
    }

    dismissNotification(loadingId);

    if (!info || !info.ok) {
      showNotification({
        title: '无法识别整合包',
        content: (info && info.error) || '未知错误',
        type: 'error',
        duration: 12000
      });
      return;
    }

    openModpackDialog(info);
  }

  function fileNameOf(path) {
    const parts = String(path).split(/[\\/]/);
    return parts[parts.length - 1] || String(path);
  }

  // 后端命令正常都会回，但万一卡住，不能让界面一直停在"正在读取"
  function withTimeout(promise, ms, message) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(message)), ms);
      promise.then(
        (value) => {
          clearTimeout(timer);
          resolve(value);
        },
        (error) => {
          clearTimeout(timer);
          reject(error);
        }
      );
    });
  }

  // 安装途中后端可能反过来问一句「内容缺失，是否继续」。后端会在这个
  // 事件上一直等回答（对应 PCL 在工作线程上阻塞的 MyMsgBox），所以
  // 无论用户怎么关，都必须回一个 answer，否则安装会挂到超时
  let modpackAskOpen = false;

  function openModpackAsk(title, message) {
    if (!modpackDialog || !modpackMask) {
      tauri.invoke('modpack.answer', { continue: false });
      return;
    }

    // 消息里每一项是 "- xxx"，剩下的都是正文
    const lines = String(message).split(/\r?\n/).map((t) => t.trim()).filter(Boolean);
    const bullets = lines.filter((t) => t.startsWith('- ')).map((t) => t.slice(2));
    const intro = lines.filter((t) => !t.startsWith('- '));

    modpackDialog.innerHTML = `
      <div class="modpack-dialog-title">${escapeHtml(title)}</div>
      <div class="modpack-name">${escapeHtml(intro[0] || '')}</div>
      <div class="modpack-warnings">${bullets.map((b) => `<div>· ${escapeHtml(b)}</div>`).join('')}</div>
      <div class="modpack-name">${escapeHtml(intro.slice(1).join(' '))}</div>
      <div class="modpack-actions">
        <button class="modpack-btn is-secondary" id="modpackAskCancel">取消安装</button>
        <button class="modpack-btn is-primary" id="modpackAskOk">继续安装</button>
      </div>`;

    modpackMask.hidden = false;
    modpackAskOpen = true;

    document.getElementById('modpackAskCancel').addEventListener('click', () => answerModpackAsk(false));
    document.getElementById('modpackAskOk').addEventListener('click', () => answerModpackAsk(true));
  }

  function answerModpackAsk(proceed) {
    if (!modpackAskOpen) return;
    modpackAskOpen = false;
    closeModpackDialog();
    tauri.invoke('modpack.answer', { continue: !!proceed });
  }

  tauri.listen('modpack.ask', (data) => {
    openModpackAsk((data && data.title) || '需要确认', (data && data.message) || '');
  });

  tauri.listen('app.fileDragEnter', () => {
    if (dropOverlay) dropOverlay.hidden = false;
  });
  tauri.listen('app.fileDragLeave', () => {
    if (dropOverlay) dropOverlay.hidden = true;
  });
  tauri.listen('app.fileDrop', (data) => {
    if (dropOverlay) dropOverlay.hidden = true;
    const paths = (data && data.paths) || [];
    if (paths.length > 0) handleModpackDrop(paths[0]);
  });

  // Esc：等确认时按取消回答；安装中 -> 取消安装；确认卡片开着 -> 关卡片
  document.addEventListener('keydown', (e) => {
    if (e.key !== 'Escape') return;
    if (modpackAskOpen) {
      answerModpackAsk(false);
      return;
    }
    if (modpackNoticeId !== null && modpackBusy) {
      tauri.invoke('modpack.cancel');
      if (modpackNoticeId !== null) {
        updateNotification(modpackNoticeId, { content: '正在取消…' });
      }
    } else if (modpackMask && !modpackMask.hidden) {
      closeModpackDialog();
    }
  });

  // ==================== 下载任务页 ====================
  // 所有下载类动作（游戏本体、整合包、资源）都在这里汇总成一个任务列表，
  // 展示速度 / 剩余大小 / 剩余时间预测。速度是前端按时间窗口算的：
  // 后端只给累计字节数，采样两次做差再平滑就够准，也省得改各服务。
  const downloadTasks = new Map();
  let downloadTaskSeq = 0;
  let tasksDomSignature = '';
  let tasksTicker = null;

  const TASK_SAMPLE_WINDOW_MS = 6000;

  function formatSize(bytes) {
    const n = Number(bytes) || 0;
    if (n <= 0) return '0 B';
    const units = ['B', 'KB', 'MB', 'GB'];
    let value = n;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
      value /= 1024;
      unit++;
    }
    return (unit === 0 ? value.toFixed(0) : value.toFixed(1)) + ' ' + units[unit];
  }

  function formatSpeed(bytesPerSecond) {
    if (!bytesPerSecond || bytesPerSecond < 1) return '—';
    return formatSize(Math.round(bytesPerSecond)) + '/s';
  }

  function formatDuration(totalSeconds) {
    const secs = Math.max(0, Math.round(totalSeconds));
    if (secs < 60) return secs + ' 秒';
    const minutes = Math.floor(secs / 60);
    if (minutes < 60) return minutes + ' 分 ' + (secs % 60) + ' 秒';
    const hours = Math.floor(minutes / 60);
    return hours + ' 时 ' + (minutes % 60) + ' 分';
  }

  // 剩余时间的显示上限。速度掉到很低时（镜像限速、连接被掐），
  // 剩余字节 / 速度会算出几十上百小时，隔一秒跳一个数，看起来像坏了。
  // 超过 24 小时一律只报 ">24 小时"
  const TASK_ETA_MAX_SECONDS = 24 * 3600;

  function formatEtaText(remainingBytes, speed) {
    if (!(speed > 0) || !(remainingBytes > 0)) return '—';
    const seconds = remainingBytes / speed;
    if (seconds >= TASK_ETA_MAX_SECONDS) return '>24 小时';
    return '约 ' + formatDuration(seconds);
  }

  // 采样窗口内的平均速度，再和上一轮做指数平滑，避免数字乱跳。
  // 注意两个坑：
  //   * 块与块之间本来就有间隔（一个 1MB 块在慢链路上要好幾秒），
  //     没有新字节时不能立刻把速度清零，否则会一直闪 0；
  //   * 阶段切换（资源→库→客户端）时字节数会归零，此时必须重置窗口。
  const TASK_SPEED_STALE_MS = 8000;

  function recomputeTaskSpeed(task) {
    const now = Date.now();
    const bytes = task.downloadedBytes || 0;
    const last = task.samples.length ? task.samples[task.samples.length - 1] : null;

    // 阶段切换：字节数回退，重置采样窗口
    if (last && bytes < last.bytes) {
      task.samples = [{ t: now, bytes }];
      task.speed = 0;
      task.lastByteAt = now;
      return;
    }

    if (!last || bytes > last.bytes) {
      task.samples.push({ t: now, bytes });
      task.lastByteAt = now;
      while (task.samples.length > 2 && now - task.samples[0].t > TASK_SAMPLE_WINDOW_MS) {
        task.samples.shift();
      }
      const first = task.samples[0];
      const dt = now - first.t;
      if (dt >= 500) {
        const raw = ((bytes - first.bytes) * 1000) / dt;
        task.speed = task.speed > 0 ? task.speed * 0.5 + raw * 0.5 : raw;
      }
      return;
    }

    // 没有新字节：先沿用上一次的值（这是正常的块间间隔），
    // 只有长时间完全没有进展才慢慢衰减
    if (task.lastByteAt && now - task.lastByteAt > TASK_SPEED_STALE_MS) {
      task.speed = (task.speed || 0) * 0.7;
    }
  }

  function createDownloadTask(options) {
    const id = ++downloadTaskSeq;
    downloadTasks.set(id, {
      id,
      kind: options.kind,
      title: options.title || '下载任务',
      subtitle: options.subtitle || '准备中…',
      phase: '',
      percent: 0,
      totalBytes: options.totalBytes || 0,
      downloadedBytes: 0,
      completedFiles: 0,
      totalFiles: 0,
      status: 'running',
      cancelCmd: options.cancelCmd || null,
      startedAt: Date.now(),
      finishedAt: 0,
      samples: [{ t: Date.now(), bytes: 0 }],
      lastByteAt: Date.now(),
      speed: 0,
      error: ''
    });
    updateTasksBadge();
    ensureTasksTicker();
    return id;
  }

  function findRunningTask(kind) {
    for (const task of downloadTasks.values()) {
      if (task.kind === kind && task.status === 'running') return task;
    }
    return null;
  }

  function updateDownloadTask(id, patch) {
    const task = id === null || id === undefined ? null : downloadTasks.get(id);
    if (!task) return;
    Object.assign(task, patch);
    recomputeTaskSpeed(task);
    // 这里不直接刷 DOM：进度事件在 64 线程下每秒能来几十上百条，
    // 每条都重排一次界面会让数字疯狂跳动、也白烧 CPU。
    // 界面统一交给 500ms 的定时器按最新状态刷新
  }

  function finishDownloadTask(id, status, message) {
    const task = id === null || id === undefined ? null : downloadTasks.get(id);
    if (!task || task.status !== 'running') return;
    task.status = status;
    task.finishedAt = Date.now();
    task.speed = 0;
    if (status === 'done') task.percent = 100;
    if (message) task.error = message;
    updateTasksBadge();
    if (isTasksPageVisible()) renderDownloadTasks(true);
  }

  function isTasksPageVisible() {
    return downloadPage.classList.contains('active') && activeCategory === 'tasks';
  }

  // 是否处于"下载任务视图"，专门用来控制进度类小通知的显隐。
  // 不能直接复用 isTasksPageVisible()：页面切换有一段 200ms 的退场/进场动画，
  // 那段时间里 downloadPage 还没加 .active，卡片会先亮出来再被藏起来。
  // 所以这里单独维护一个状态位，由所有改变当前视图的入口主动设置。
  let tasksViewActive = false;

  function setTasksViewActive(active) {
    tasksViewActive = active;
    document.body.classList.toggle('is-tasks-page', active);
  }

  function updateTasksBadge() {
    const badge = document.getElementById('tasksBadge');
    if (!badge) return;
    let running = 0;
    for (const task of downloadTasks.values()) {
      if (task.status === 'running') running++;
    }
    badge.hidden = running === 0;
    badge.textContent = running > 0 ? String(running) : '';
  }

  // 有任务在跑时才起定时器；速度是靠时间窗口滚动的，没有新事件也得重算
  function ensureTasksTicker() {
    if (tasksTicker) return;
    tasksTicker = setInterval(() => {
      let hasRunning = false;
      for (const task of downloadTasks.values()) {
        if (task.status !== 'running') continue;
        hasRunning = true;
        recomputeTaskSpeed(task);
      }
      if (!hasRunning) {
        clearInterval(tasksTicker);
        tasksTicker = null;
      }
      updateTasksBadge();
      if (isTasksPageVisible()) renderDownloadTasks(false);
    }, 500);
  }

  function cancelDownloadTask(id) {
    const task = downloadTasks.get(id);
    if (!task || task.status !== 'running' || !task.cancelCmd) return;
    task.subtitle = '正在取消…';
    tauri.invoke(task.cancelCmd, {}).catch(() => {});
    if (isTasksPageVisible()) renderDownloadTasks(false);
  }

  // 自动跳到下载任务页。已经在「下载」页时只切子分类，不走整页切换动画
  function openTasksPage() {
    if (downloadPage.classList.contains('active')) {
      activeCategory = 'tasks';
      subSidebarItems.forEach(i => i.classList.toggle('active', i.dataset.category === 'tasks'));
      loadDownloadContent('tasks');
      setTasksViewActive(true);
      return;
    }
    pendingDownloadCategory = 'tasks';
    // 先置位再切页：整页切换有 200ms 动画，这里不提前置位的话，
    // 紧接着创建的那张进度通知会先亮出来再被藏掉，闪一下很难看
    setTasksViewActive(true);
    switchPageWithSidebar('download');
  }

  const TASK_STATUS_LABELS = {
    running: ['is-running', '进行中'],
    done: ['is-done', '已完成'],
    failed: ['is-failed', '失败'],
    cancelled: ['is-cancelled', '已取消']
  };

  function taskCardHtml(task) {
    const badge = TASK_STATUS_LABELS[task.status] || TASK_STATUS_LABELS.running;
    const cardClass = task.status === 'running' ? ' is-running'
      : task.status === 'done' ? ' is-done'
      : task.status === 'failed' ? ' is-failed' : '';
    const cancel = task.status === 'running' && task.cancelCmd
      ? `<div class="task-actions"><button class="task-btn is-danger" data-action="cancel" data-task-id="${task.id}">取消下载</button></div>`
      : '';
    return `
      <div class="task-card${cardClass}" data-task-id="${task.id}">
        <div class="task-card-top">
          <div class="task-card-titles">
            <div class="task-title" title="${escapeHtml(task.title)}">${escapeHtml(task.title)}</div>
            <div class="task-sub" data-role="sub"></div>
          </div>
          <span class="task-badge ${badge[0]}">${badge[1]}</span>
        </div>
        <div class="task-bar"><div class="task-bar-fill" data-role="fill"></div></div>
        <div class="task-stats">
          <div class="task-stat"><span class="task-stat-label">速度</span><span class="task-stat-value" data-role="speed">—</span></div>
          <div class="task-stat"><span class="task-stat-label">已下载 / 总量</span><span class="task-stat-value" data-role="size">—</span></div>
          <div class="task-stat"><span class="task-stat-label">剩余大小</span><span class="task-stat-value" data-role="remain">—</span></div>
          <div class="task-stat"><span class="task-stat-label">剩余时间</span><span class="task-stat-value" data-role="eta">—</span></div>
        </div>
        ${cancel}
      </div>`;
  }

  function tasksSignature() {
    const parts = [];
    for (const task of downloadTasks.values()) parts.push(task.id + ':' + task.status);
    return parts.join('|');
  }

  function renderDownloadTasks(force) {
    if (!downloadContent) return;

    const signature = tasksSignature();
    if (!force && signature === tasksDomSignature) {
      patchTasksDom();
      return;
    }
    tasksDomSignature = signature;
    // 任务页接管了内容区：作废在途的版本列表 / 资源浏览请求，
    // 否则它们的响应回来会把任务列表覆盖掉
    downloadViewSeq++;

    if (downloadTasks.size === 0) {
      downloadContent.innerHTML = `
        <div class="tasks-page">
          <div class="tasks-head"><div class="tasks-title">下载任务</div></div>
          <div class="tasks-empty">
            <div>暂无下载任务</div>
            <div>下载游戏、整合包或资源时，进度会显示在这里</div>
          </div>
        </div>`;
      setTasksViewActive(true);
      return;
    }

    const running = [];
    const finished = [];
    for (const task of downloadTasks.values()) {
      (task.status === 'running' ? running : finished).push(task);
    }
    const newestFirst = (a, b) => b.id - a.id;
    running.sort(newestFirst);
    finished.sort(newestFirst);

    let html = `
      <div class="tasks-page">
        <div class="tasks-head">
          <div class="tasks-title">下载任务</div>
          <div class="tasks-summary" id="tasksSummary"></div>
        </div>`;
    if (running.length) {
      html += `<div class="tasks-list">${running.map(taskCardHtml).join('')}</div>`;
    }
    if (finished.length) {
      html += `<div class="tasks-section-title">已结束</div>`;
      html += `<div class="tasks-list">${finished.map(taskCardHtml).join('')}</div>`;
    }
    html += `</div>`;
    downloadContent.innerHTML = html;

    downloadContent.querySelectorAll('[data-action="cancel"]').forEach((btn) => {
      btn.addEventListener('click', () => cancelDownloadTask(Number(btn.dataset.taskId)));
    });

    patchTasksDom();
    setTasksViewActive(true);
  }

  function setTaskStat(card, role, text) {
    const el = card.querySelector(`[data-role="${role}"]`);
    if (el) el.textContent = text;
  }

  function patchTasksDom() {
    if (!downloadContent) return;

    const summary = document.getElementById('tasksSummary');
    if (summary) {
      let running = 0;
      let speed = 0;
      for (const task of downloadTasks.values()) {
        if (task.status !== 'running') continue;
        running++;
        speed += task.speed || 0;
      }
      summary.textContent = running > 0
        ? `${running} 个进行中 · 总速度 ${formatSpeed(speed)}`
        : `${downloadTasks.size} 个任务`;
    }

    for (const task of downloadTasks.values()) {
      const card = downloadContent.querySelector(`.task-card[data-task-id="${task.id}"]`);
      if (!card) continue;

      const fill = card.querySelector('[data-role="fill"]');
      if (fill) {
        const percent = Math.max(0, Math.min(100, task.status === 'done' ? 100 : task.percent || 0));
        fill.style.transform = 'scaleX(' + percent / 100 + ')';
      }

      const sub = card.querySelector('[data-role="sub"]');
      if (sub) {
        sub.textContent = task.status === 'running'
          ? (task.subtitle || task.phase || '')
          : (task.error || task.subtitle || '');
      }

      if (task.status !== 'running') {
        setTaskStat(card, 'speed', '—');
        setTaskStat(card, 'eta', '—');
      } else {
        setTaskStat(card, 'speed', formatSpeed(task.speed));
        const remaining = task.totalBytes > 0
          ? Math.max(0, task.totalBytes - (task.downloadedBytes || 0))
          : 0;
        if (remaining <= 0 && task.totalBytes > 0) {
          setTaskStat(card, 'eta', '即将完成');
        } else {
          setTaskStat(card, 'eta', formatEtaText(remaining, task.speed));
        }
      }

      const downloaded = task.downloadedBytes || 0;
      if (task.totalBytes > 0) {
        setTaskStat(card, 'size', formatSize(downloaded) + ' / ' + formatSize(task.totalBytes));
        const remaining = Math.max(0, task.totalBytes - downloaded);
        setTaskStat(card, 'remain', task.status === 'running' ? formatSize(remaining) : '—');
      } else if (task.totalFiles > 0) {
        // 后端没给字节数（例如整合包文件体积未知）时退回文件计数
        setTaskStat(card, 'size', `${task.completedFiles}/${task.totalFiles} 个文件`);
        setTaskStat(card, 'remain', '—');
      } else if (downloaded > 0) {
        setTaskStat(card, 'size', formatSize(downloaded));
        setTaskStat(card, 'remain', '—');
      } else {
        setTaskStat(card, 'size', '—');
        setTaskStat(card, 'remain', '—');
      }
    }
  }

  // 整合包安装进入"装原版"阶段后，真正的字节进度是 DownloadService 在报
  // （download.progress），ModpackService 只在阶段切换时给一个总百分比。
  // 不接这条事件的话，任务页会在整个资源下载期间定死在 65%，
  // 速度 / 剩余大小 / 剩余时间全是 "—"，看起来就像卡死 —— 其实数据一直在流动。
  const VANILLA_SUBPHASE_LABELS = {
    version: '版本信息',
    assets: '资源文件',
    libraries: '库文件',
    client: '客户端'
  };
  // 装原版在总进度里占 65%~88%。各区间的端点必须和后端阶段步进
  // （PercentAt("vanilla", 20/50/80/100) = 69/76/83/88）对齐，否则
  // 后端一报"正在下载客户端"（83%）前端却从 85% 起算，进度条会往上跳一下
  const VANILLA_SUBPHASE_RANGE = {
    assets: [69, 76],
    libraries: [76, 83],
    client: [83, 88]
  };

  tauri.listen('download.progress', (progress) => {
    if (!progress) return;
    const task = findRunningTask('modpack');
    // 只管装原版阶段：解压和下载整合包内容由 modpack.progress 自己报
    if (!task || task.phase !== 'vanilla') return;

    const range = VANILLA_SUBPHASE_RANGE[progress.phase];
    if (!range) return;

    const total = progress.totalBytes || 0;
    const downloaded = progress.downloadedBytes || 0;
    const totalFiles = progress.totalFiles || 0;
    const completedFiles = progress.completedFiles || 0;

    let subtitle = '正在下载游戏' + (VANILLA_SUBPHASE_LABELS[progress.phase] || '');
    if (totalFiles > 0) subtitle += ' ' + completedFiles + '/' + totalFiles;

    task.subtitle = subtitle;
    task.totalBytes = total;
    task.downloadedBytes = downloaded;
    task.completedFiles = completedFiles;
    task.totalFiles = totalFiles;

    // 同一个子阶段内字节数会临时回退（分段下载某块失败要把它已经计入的
    // 字节扣回去，然后重试），所以子阶段内的比例只许涨不许跌
    if (task.subPhase !== progress.phase) {
      task.subPhase = progress.phase;
      task.subRatio = 0;
    }
    const ratio = total > 0 ? Math.min(1, downloaded / total) : 0;
    if (ratio > task.subRatio) task.subRatio = ratio;

    const percent = Math.round(range[0] + (range[1] - range[0]) * task.subRatio);
    task.percent = Math.max(task.percent || 0, percent);
  });

  // ---- 展开入场 ----
  // display / [hidden] 的切换本身没有过渡，浏览器不会补间，所以重新显示时挂一次类，
  // 靠 keyframes 播一遍「淡入 + 轻微位移」；收起保持瞬时，不做退场。
  // 注意：面板换内容是「新元素插入」，CSS 会自己播动画，不需要在这里重挂类，
  // 那个场景由面板上的 .stagger-in 负责（见 style.css）。
  function replayReveal(el) {
    if (!el) return;
    el.classList.remove('is-revealing');
    void el.offsetWidth;
    el.classList.add('is-revealing');
  }

  // 暴露到全局
  window.showNotification = showNotification;
  window.dismissNotification = dismissNotification;
  window.clearAllNotifications = clearAllNotifications;

})();