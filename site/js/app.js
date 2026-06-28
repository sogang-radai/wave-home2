async function fetchJson(url) {
    const response = await fetch(url);
    if (!response.ok) {
        throw new Error(`HTTP ${response.status}`);
    }
    return response.json();
}

function setStatus(text, ok) {
    const el = document.getElementById("status-text");
    el.textContent = text;
    el.className = ok ? "ok" : "error";
}

function renderApps(apps) {
    const list = document.getElementById("app-list");
    list.innerHTML = "";

    if (!apps.length) {
        const item = document.createElement("li");
        item.textContent = "등록된 앱이 없습니다.";
        list.appendChild(item);
        return;
    }

    for (const app of apps) {
        const item = document.createElement("li");
        item.innerHTML =
            `<div class="app-name">${app.name}</div>` +
            `<div class="app-desc">${app.description}</div>`;
        list.appendChild(item);
    }
}

async function refreshDashboard() {
    setStatus("확인 중...", false);

    try {
        const status = await fetchJson("/api/status");
        setStatus(`${status.message} (v${status.version})`, true);

        const data = await fetchJson("/api/apps");
        renderApps(data.apps);
    } catch (err) {
        setStatus(`서버 연결 실패: ${err.message}`, false);
        renderApps([]);
    }
}

document.getElementById("refresh-btn").addEventListener("click", refreshDashboard);
refreshDashboard();
