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
  public bg: string[] = new Array(CoreMapComponent.N).fill('rgb(22,32,46)');
  public fg: string[] = new Array(CoreMapComponent.N).fill('rgba(231,238,245,0.55)');
  public glow: string[] = new Array(CoreMapComponent.N).fill('none');
  // Déphasage de la "respiration" par cœur -> champ vivant, pas un clignotement synchronisé.
  public delays: string[] = Array.from({ length: CoreMapComponent.N }, (_, i) => '-' + ((i * 0.371) % 2.4).toFixed(2) + 's');
  public total = 0;
  public activeCount = 0;
  public maxCount = 0;
  public hotCore = -1;

  private flash: number[] = new Array(CoreMapComponent.N).fill(0);
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
    this.decayTimer = setInterval(() => this.decay(), 120);
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
    this.total++;
    this.flash[c] = 1;
    this.glow[c] = this.glowFor(1);

    if (this.counts[c] > this.maxCount) {
      this.maxCount = this.counts[c];
      this.hotCore = c;
      this.repaintAll();          // le max a bougé -> toute l'échelle de chaleur change
    } else {
      this.paint(c);
    }
  }

  // Base persistante : teinte = nombre CUMULÉ de solutions du coeur (donnee reelle, pas un clignotement).
  private paint(i: number): void {
    const t = this.maxCount ? this.counts[i] / this.maxCount : 0;
    this.bg[i] = this.heatColor(t);
    this.fg[i] = t > 0.55 ? 'rgba(16,22,12,0.9)' : 'rgba(231,238,245,0.82)';
  }

  private repaintAll(): void {
    for (let i = 0; i < CoreMapComponent.N; i++) {
      this.paint(i);
    }
  }

  // Éclair bref quand un coeur vient de TROUVER une solution, par-dessus la base.
  private decay(): void {
    for (let i = 0; i < CoreMapComponent.N; i++) {
      if (this.flash[i] > 0.02) {
        this.flash[i] *= 0.82;
        this.glow[i] = this.glowFor(this.flash[i]);
      } else if (this.flash[i] !== 0) {
        this.flash[i] = 0;
        this.glow[i] = 'none';
      }
    }
  }

  private glowFor(f: number): string {
    if (f <= 0.02) {
      return 'none';
    }
    return `0 0 ${Math.round(2 + 8 * f)}px rgba(255,224,138,${(0.15 + 0.55 * f).toFixed(2)})`;
  }

  // Dégradé thermique : froid (ardoise) -> chaud (ambre) selon le nombre cumulé.
  private heatColor(t: number): string {
    const x = Math.min(1, Math.max(0, t));
    const stops = [[22, 32, 46], [42, 74, 85], [65, 214, 138], [240, 178, 60]];
    const p = x * (stops.length - 1);
    const i = Math.min(stops.length - 2, Math.floor(p));
    const f = p - i;
    const a = stops[i];
    const b = stops[i + 1];
    const r = Math.round(a[0] + (b[0] - a[0]) * f);
    const g = Math.round(a[1] + (b[1] - a[1]) * f);
    const bl = Math.round(a[2] + (b[2] - a[2]) * f);
    return `rgb(${r},${g},${bl})`;
  }
}
