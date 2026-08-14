import { Component, OnDestroy, OnInit } from '@angular/core';
import { Subscription } from 'rxjs';
import { WebsocketService } from 'src/app/services/web-socket.service';

interface AsicResult {
  core: string;
  nonce: string;
  diff: string;
  diffNum: number;
  color: string;
  width: number;    // largeur (%) de la barre de magnitude
  top: boolean;     // plus forte des solutions visibles
  record: boolean;  // égale le record de session (moment fort)
}

@Component({
  selector: 'app-asic-live',
  templateUrl: './asic-live.component.html',
  styleUrl: './asic-live.component.scss'
})
export class AsicLiveComponent implements OnInit, OnDestroy {

  public results: AsicResult[] = [];
  public bestDiff = 0;
  public bestDiffLabel = '--';
  public perMin = 0;

  private sub?: Subscription;
  private buffer = '';
  private times: number[] = [];

  constructor(private websocketService: WebsocketService) {}

  ngOnInit(): void {
    this.sub = this.websocketService.ws$.subscribe({
      next: (val: string) => {
        this.buffer += val;
        let idx: number;
        while ((idx = this.buffer.indexOf('\n')) >= 0) {
          const line = this.buffer.slice(0, idx);
          this.buffer = this.buffer.slice(idx + 1);
          this.parseLine(line);
        }
      },
      error: () => { /* la carte reste simplement vide */ }
    });
  }

  ngOnDestroy(): void {
    this.sub?.unsubscribe();
  }

  // Ligne type : "... asic_result: ID: ..., Core: X/Y, ver: ABCD Nonce 12AB34CD diff 15234.5 of 1000."
  private parseLine(line: string): void {
    const m = line.match(/asic_result:.*Core: (\d+)\/(\d+).*Nonce ([0-9A-Fa-f]+) diff ([\d.]+)/);
    if (!m) {
      return;
    }
    const diffNum = parseFloat(m[4]);
    if (isNaN(diffNum)) {
      return;
    }
    if (diffNum > this.bestDiff) {
      this.bestDiff = diffNum;
      this.bestDiffLabel = this.formatDiff(diffNum);
    }
    this.results.unshift({
      core: `${m[1]}/${m[2]}`,
      nonce: m[3].toLowerCase(),
      diff: this.formatDiff(diffNum),
      diffNum,
      color: '#6f8a9e',
      width: 8,
      top: false,
      record: false
    });
    if (this.results.length > 8) {
      this.results.pop();
    }
    this.updateRate();
    this.recompute();
  }

  private updateRate(): void {
    const now = Date.now();
    this.times.push(now);
    while (this.times.length && now - this.times[0] > 60000) {
      this.times.shift();
    }
    this.perMin = this.times.length;
  }

  // Couleur + magnitude relatives à la fenêtre visible (échelle log) : on voit d'un coup
  // d'œil la hiérarchie des solutions récentes, le record ressort en or.
  private recompute(): void {
    if (!this.results.length) {
      return;
    }
    let min = Infinity;
    let max = 0;
    for (const r of this.results) {
      if (r.diffNum < min) { min = r.diffNum; }
      if (r.diffNum > max) { max = r.diffNum; }
    }
    const lo = Math.log(Math.max(1, min));
    const span = (Math.log(Math.max(1, max)) - lo) || 1;
    for (const r of this.results) {
      const t = (Math.log(Math.max(1, r.diffNum)) - lo) / span;
      r.width = Math.round(8 + t * 92);
      r.record = r.diffNum === this.bestDiff && this.bestDiff > 0;
      r.top = r.diffNum === max && !r.record;
      r.color = r.record ? '#ffe08a' : this.scale(t);
    }
  }

  // Ardoise -> cyan -> vert -> ambre -> or, selon la force relative.
  private scale(t: number): string {
    if (t < 0.22) { return '#6f8a9e'; }
    if (t < 0.45) { return '#35d0c0'; }
    if (t < 0.72) { return '#41d68a'; }
    if (t < 0.92) { return '#f0b23c'; }
    return '#ffe08a';
  }

  private formatDiff(v: number): string {
    if (isNaN(v)) return '--';
    if (v >= 1e12) return (v / 1e12).toFixed(1) + ' T';
    if (v >= 1e9) return (v / 1e9).toFixed(1) + ' G';
    if (v >= 1e6) return (v / 1e6).toFixed(1) + ' M';
    if (v >= 1e3) return (v / 1e3).toFixed(1) + ' K';
    return v.toFixed(0);
  }
}
