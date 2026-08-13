import { Component, OnDestroy, OnInit } from '@angular/core';
import { Subscription } from 'rxjs';
import { WebsocketService } from 'src/app/services/web-socket.service';

@Component({
  selector: 'app-core-map',
  templateUrl: './core-map.component.html',
  styleUrl: './core-map.component.scss'
})
export class CoreMapComponent implements OnInit, OnDestroy {

  private static readonly N = 128;

  public cells: number[] = Array.from({ length: CoreMapComponent.N }, (_, i) => i);
  public counts: number[] = new Array(CoreMapComponent.N).fill(0);
  public bg: string[] = new Array(CoreMapComponent.N).fill('rgba(100,116,139,0.12)');
  public total = 0;
  public activeCount = 0;

  private heat: number[] = new Array(CoreMapComponent.N).fill(0);
  private sub?: Subscription;
  private buffer = '';
  private decayTimer?: any;

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
      error: () => { /* la carte reste froide */ }
    });
    this.decayTimer = setInterval(() => this.decay(), 150);
  }

  ngOnDestroy(): void {
    this.sub?.unsubscribe();
    if (this.decayTimer) {
      clearInterval(this.decayTimer);
    }
  }

  public trackIdx(index: number): number {
    return index;
  }

  // Ligne type : "... asic_result: ... Core: 91/9, ver: ..."  -> on extrait le coeur (0-127)
  private parseLine(line: string): void {
    const m = line.match(/asic_result:.*Core: (\d+)\/\d+/);
    if (!m) {
      return;
    }
    const c = parseInt(m[1], 10);
    if (isNaN(c) || c < 0 || c >= CoreMapComponent.N) {
      return;
    }
    if (this.counts[c] === 0) {
      this.activeCount++;   // premier hit de ce coeur -> il "existe" reellement
    }
    this.counts[c]++;
    this.heat[c] = 1;
    this.total++;
  }

  private decay(): void {
    for (let i = 0; i < CoreMapComponent.N; i++) {
      if (this.heat[i] > 0.01) {
        this.heat[i] *= 0.88;
        this.bg[i] = this.color(this.heat[i]);
      } else if (this.heat[i] !== 0) {
        this.heat[i] = 0;
        this.bg[i] = this.color(0);
      }
    }
  }

  // Dégradé thermique : froid (ardoise) -> chaud (ambre)
  private color(h: number): string {
    const t = Math.min(1, Math.max(0, h));
    const r = Math.round(100 + 149 * t);
    const g = Math.round(116 + 52 * t);
    const b = Math.round(139 - 102 * t);
    const a = (0.12 + 0.85 * t).toFixed(2);
    return `rgba(${r},${g},${b},${a})`;
  }
}
