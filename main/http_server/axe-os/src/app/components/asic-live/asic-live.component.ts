import { Component, OnDestroy, OnInit } from '@angular/core';
import { Subscription } from 'rxjs';
import { WebsocketService } from 'src/app/services/web-socket.service';

interface AsicResult {
  core: string;
  nonce: string;
  diff: string;
}

@Component({
  selector: 'app-asic-live',
  templateUrl: './asic-live.component.html',
  styleUrl: './asic-live.component.scss'
})
export class AsicLiveComponent implements OnInit, OnDestroy {

  public results: AsicResult[] = [];

  private sub?: Subscription;
  private buffer = '';

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
    this.results.unshift({
      core: `${m[1]}/${m[2]}`,
      nonce: m[3].toLowerCase(),
      diff: this.formatDiff(parseFloat(m[4]))
    });
    if (this.results.length > 8) {
      this.results.pop();
    }
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
