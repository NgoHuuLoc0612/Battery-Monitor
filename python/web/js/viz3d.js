/**
 * Battery Monitor — 3D Visualization Module
 * Three.js scene: battery cell array, charge fill animation,
 * thermal heat map via vertex colors, voltage particle field,
 * auto-rotate with live telemetry mapping
 */

'use strict';

(function () {
  let scene, camera, renderer, animId;
  let batteryGroup, cells = [], heatMap;
  let chargeLevel = 0.85;
  let thermalRisk = 0;
  let isInitialized = false;
  let lastSnap = null;
  let particles, particlePositions, particleVelocities;

  const CELL_ROWS = 4;
  const CELL_COLS = 4;
  const CELL_W = 0.7, CELL_H = 1.8, CELL_D = 0.4;
  const CELL_GAP = 0.15;

  // ─── Init ──────────────────────────────────────────────────────────────────
  function init3D() {
    const canvas = document.getElementById('canvas3d');
    if (!canvas || isInitialized) return;
    isInitialized = true;

    // Scene
    scene = new THREE.Scene();
    scene.background = new THREE.Color(0x04080e);
    scene.fog = new THREE.FogExp2(0x04080e, 0.08);

    // Camera
    camera = new THREE.PerspectiveCamera(50, canvas.clientWidth / canvas.clientHeight, 0.1, 100);
    camera.position.set(0, 3, 9);
    camera.lookAt(0, 0, 0);

    // Renderer
    renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: false });
    renderer.setSize(canvas.clientWidth, canvas.clientHeight);
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    renderer.shadowMap.enabled = true;
    renderer.shadowMap.type = THREE.PCFSoftShadowMap;

    // Lighting
    const ambient = new THREE.AmbientLight(0x0a1a2a, 3.0);
    scene.add(ambient);

    const keyLight = new THREE.DirectionalLight(0x00e5ff, 1.5);
    keyLight.position.set(5, 8, 5);
    keyLight.castShadow = true;
    scene.add(keyLight);

    const fillLight = new THREE.PointLight(0x003355, 2.0, 20);
    fillLight.position.set(-4, 2, -2);
    scene.add(fillLight);

    const rimLight = new THREE.PointLight(0x002244, 1.5, 15);
    rimLight.position.set(0, -3, -5);
    scene.add(rimLight);

    // ─── Grid floor ────────────────────────────────────────────────────────
    const gridHelper = new THREE.GridHelper(20, 40, 0x1e2d3e, 0x111820);
    gridHelper.position.y = -2.8;
    scene.add(gridHelper);

    // ─── Battery pack group ────────────────────────────────────────────────
    batteryGroup = new THREE.Group();
    scene.add(batteryGroup);

    buildCells();
    buildPackHousing();
    buildParticleField();
    buildConnectors();

    // ─── Resize handler ────────────────────────────────────────────────────
    window.addEventListener('resize', onResize3D);

    // ─── Mouse orbit (manual, no OrbitControls dependency) ─────────────────
    setupMouseOrbit(canvas);

    animate();
  }

  // ─── Build battery cells ───────────────────────────────────────────────────
  function buildCells() {
    const totalW = CELL_COLS * (CELL_W + CELL_GAP) - CELL_GAP;
    const totalD = CELL_ROWS * (CELL_D + CELL_GAP) - CELL_GAP;

    for (let row = 0; row < CELL_ROWS; row++) {
      for (let col = 0; col < CELL_COLS; col++) {
        const x = (col * (CELL_W + CELL_GAP)) - totalW / 2 + CELL_W / 2;
        const z = (row * (CELL_D + CELL_GAP)) - totalD / 2 + CELL_D / 2;

        // Cell body
        const cellGeo = new THREE.BoxGeometry(CELL_W, CELL_H, CELL_D, 1, 8, 1);
        const cellMat = new THREE.MeshStandardMaterial({
          color: 0x1a2a3a,
          metalness: 0.8,
          roughness: 0.3,
          emissive: 0x001122,
          emissiveIntensity: 0.5,
        });
        const cellMesh = new THREE.Mesh(cellGeo, cellMat);
        cellMesh.position.set(x, 0, z);
        cellMesh.castShadow = true;
        cellMesh.receiveShadow = true;
        batteryGroup.add(cellMesh);

        // Charge fill plane (inside cell)
        const fillGeo = new THREE.BoxGeometry(CELL_W * 0.85, CELL_H * 0.02, CELL_D * 0.85);
        const fillMat = new THREE.MeshStandardMaterial({
          color: 0x00e5ff,
          emissive: 0x00e5ff,
          emissiveIntensity: 1.5,
          transparent: true,
          opacity: 0.9,
        });
        const fillMesh = new THREE.Mesh(fillGeo, fillMat);
        fillMesh.position.set(x, -CELL_H / 2 * (1 - chargeLevel * 2) + CELL_H / 2, z);
        batteryGroup.add(fillMesh);

        // Solid charge column
        const colGeo = new THREE.BoxGeometry(CELL_W * 0.85, CELL_H * chargeLevel, CELL_D * 0.85);
        const colMat = new THREE.MeshStandardMaterial({
          color: 0x00b8cc,
          emissive: 0x003344,
          emissiveIntensity: 0.8,
          transparent: true,
          opacity: 0.55,
        });
        const colMesh = new THREE.Mesh(colGeo, colMat);
        colMesh.position.set(x, -CELL_H / 2 + (CELL_H * chargeLevel) / 2, z);
        batteryGroup.add(colMesh);

        cells.push({
          body: cellMesh, fill: fillMesh, col: colMesh,
          baseX: x, baseZ: z,
          row, colIdx: col,
          heatTarget: 0, heatCurrent: 0,
        });
      }
    }
    batteryGroup.position.y = -0.4;
  }

  // ─── Pack housing (transparent shell) ─────────────────────────────────────
  function buildPackHousing() {
    const geo = new THREE.BoxGeometry(
      CELL_COLS * (CELL_W + CELL_GAP) + 0.3,
      CELL_H + 0.3,
      CELL_ROWS * (CELL_D + CELL_GAP) + 0.3
    );
    const mat = new THREE.MeshStandardMaterial({
      color: 0x001520,
      metalness: 0.9,
      roughness: 0.1,
      transparent: true,
      opacity: 0.12,
      side: THREE.BackSide,
    });
    const housing = new THREE.Mesh(geo, mat);
    housing.position.y = -0.4;
    scene.add(housing);

    // Edge wireframe
    const edgesGeo = new THREE.EdgesGeometry(geo);
    const edgesMat = new THREE.LineBasicMaterial({ color: 0x1e2d3e, linewidth: 1 });
    const wireframe = new THREE.LineSegments(edgesGeo, edgesMat);
    wireframe.position.y = -0.4;
    scene.add(wireframe);
  }

  // ─── Connectors between cells ──────────────────────────────────────────────
  function buildConnectors() {
    const mat = new THREE.MeshStandardMaterial({
      color: 0x334455,
      metalness: 1.0,
      roughness: 0.2,
    });
    for (let row = 0; row < CELL_ROWS; row++) {
      for (let col = 0; col < CELL_COLS - 1; col++) {
        const a = cells[row * CELL_COLS + col];
        const b = cells[row * CELL_COLS + col + 1];
        const midX = (a.baseX + b.baseX) / 2;
        const geo = new THREE.BoxGeometry(CELL_GAP + 0.02, 0.06, 0.1);
        const mesh = new THREE.Mesh(geo, mat);
        mesh.position.set(midX, CELL_H / 2 - 0.4 + 0.03, a.baseZ);
        batteryGroup.add(mesh);
      }
    }
  }

  // ─── Particle field (charge ions visualization) ────────────────────────────
  function buildParticleField() {
    const count = 600;
    const geo = new THREE.BufferGeometry();
    particlePositions = new Float32Array(count * 3);
    particleVelocities = [];

    for (let i = 0; i < count; i++) {
      particlePositions[i * 3 + 0] = (Math.random() - 0.5) * 8;
      particlePositions[i * 3 + 1] = (Math.random() - 0.5) * 5;
      particlePositions[i * 3 + 2] = (Math.random() - 0.5) * 6;
      particleVelocities.push(
        (Math.random() - 0.5) * 0.005,
        (Math.random() - 0.5) * 0.005 + 0.003,
        (Math.random() - 0.5) * 0.005
      );
    }

    geo.setAttribute('position', new THREE.BufferAttribute(particlePositions, 3));
    const mat = new THREE.PointsMaterial({
      color: 0x00e5ff,
      size: 0.04,
      transparent: true,
      opacity: 0.5,
      blending: THREE.AdditiveBlending,
      depthWrite: false,
    });
    particles = new THREE.Points(geo, mat);
    scene.add(particles);
  }

  // ─── Mouse orbit ───────────────────────────────────────────────────────────
  let mouseDown = false, lastMX = 0, lastMY = 0;
  let rotY = 0, rotX = 0.2;

  function setupMouseOrbit(canvas) {
    canvas.addEventListener('mousedown', e => { mouseDown = true; lastMX = e.clientX; lastMY = e.clientY; });
    canvas.addEventListener('mouseup', () => { mouseDown = false; });
    canvas.addEventListener('mousemove', e => {
      if (!mouseDown) return;
      rotY += (e.clientX - lastMX) * 0.005;
      rotX += (e.clientY - lastMY) * 0.003;
      rotX = Math.max(-0.6, Math.min(0.8, rotX));
      lastMX = e.clientX; lastMY = e.clientY;
    });
    canvas.addEventListener('wheel', e => {
      camera.position.z += e.deltaY * 0.01;
      camera.position.z = Math.max(4, Math.min(16, camera.position.z));
    });
  }

  // ─── Update from telemetry ─────────────────────────────────────────────────
  function update3D(snap) {
    if (!isInitialized) return;
    lastSnap = snap;

    chargeLevel = Math.max(0.01, Math.min(1.0, (snap.soc_accurate || snap.percent_remaining || 50) / 100));
    thermalRisk = (snap.thermal_risk_index || 0) / 100;

    // Update HUD
    const setHud = (id, v) => { const el = document.getElementById(id); if (el) el.textContent = v; };
    setHud('hud3dSoc',   `${(chargeLevel * 100).toFixed(1)}%`);
    setHud('hud3dVolt',  `${(snap.voltage || 0).toFixed(3)}V`);
    setHud('hud3dTemp',  `${(snap.temperature_celsius || 0).toFixed(1)}°C`);
    setHud('hud3dPower', `${(snap.power_now || 0).toFixed(2)}W`);

    // Distribute heat: center cells hotter
    cells.forEach((cell, i) => {
      const row = cell.row, col = cell.colIdx;
      const dr = Math.abs(row - 1.5) / 2, dc = Math.abs(col - 1.5) / 2;
      const heatFactor = (1 - (dr + dc) / 2) * thermalRisk;
      cell.heatTarget = heatFactor;
    });

    // Particle color: charge vs discharge
    if (particles) {
      const color = snap.charging
        ? new THREE.Color(0x00e5ff)
        : new THREE.Color(0xffb300);
      particles.material.color.lerp(color, 0.05);
      particles.material.opacity = 0.3 + chargeLevel * 0.4;
    }
  }

  // ─── Animation loop ────────────────────────────────────────────────────────
  let t = 0;
  function animate() {
    animId = requestAnimationFrame(animate);
    t += 0.016;

    // Auto-rotate (slow)
    batteryGroup.rotation.y = rotY + t * 0.15;
    batteryGroup.rotation.x = rotX;

    // Update cells
    cells.forEach((cell, i) => {
      // Smooth heat
      cell.heatCurrent += (cell.heatTarget - cell.heatCurrent) * 0.02;

      // Cell color: cool=blue, hot=red
      const heatColor = new THREE.Color().lerpColors(
        new THREE.Color(0x1a2a3a),
        new THREE.Color(0x8b1a1a),
        cell.heatCurrent
      );
      cell.body.material.color.lerp(heatColor, 0.05);
      cell.body.material.emissiveIntensity = 0.3 + cell.heatCurrent * 1.5;
      cell.body.material.emissive.setHex(cell.heatCurrent > 0.3 ? 0x2a0500 : 0x001122);

      // Charge column height
      const targetY = -CELL_H / 2 + (CELL_H * chargeLevel) / 2;
      cell.col.position.y = THREE.MathUtils.lerp(cell.col.position.y, targetY, 0.03);
      cell.col.scale.y = THREE.MathUtils.lerp(
        cell.col.scale.y,
        chargeLevel,
        0.03
      );

      // Fill plane
      const fillY = -CELL_H / 2 + CELL_H * chargeLevel;
      cell.fill.position.y = THREE.MathUtils.lerp(cell.fill.position.y, fillY + batteryGroup.position.y, 0.03);

      // Pulsing glow on charge
      if (chargeLevel > 0.05) {
        const pulse = 0.8 + 0.4 * Math.sin(t * 2.5 + i * 0.3);
        cell.col.material.emissiveIntensity = pulse * 0.5 * chargeLevel;
      }
    });

    // Particle animation
    if (particles && particlePositions && particleVelocities) {
      const count = particlePositions.length / 3;
      for (let i = 0; i < count; i++) {
        particlePositions[i * 3 + 0] += particleVelocities[i * 3 + 0];
        particlePositions[i * 3 + 1] += particleVelocities[i * 3 + 1];
        particlePositions[i * 3 + 2] += particleVelocities[i * 3 + 2];

        // Wrap
        if (particlePositions[i * 3 + 1] > 3) particlePositions[i * 3 + 1] = -2.5;
        if (Math.abs(particlePositions[i * 3 + 0]) > 5) particleVelocities[i * 3 + 0] *= -1;
        if (Math.abs(particlePositions[i * 3 + 2]) > 4) particleVelocities[i * 3 + 2] *= -1;
      }
      particles.geometry.attributes.position.needsUpdate = true;
    }

    renderer.render(scene, camera);
  }

  function onResize3D() {
    const canvas = document.getElementById('canvas3d');
    if (!canvas || !renderer) return;
    camera.aspect = canvas.clientWidth / canvas.clientHeight;
    camera.updateProjectionMatrix();
    renderer.setSize(canvas.clientWidth, canvas.clientHeight);
  }

  function destroy3D() {
    if (animId) cancelAnimationFrame(animId);
    if (renderer) renderer.dispose();
    isInitialized = false;
  }

  window.Viz3D = { init3D, update3D, destroy3D };
})();
