import { Component, OnDestroy, OnInit } from '@angular/core';
import { SystemApiService } from 'src/app/services/system.service';

@Component({
  selector: 'app-rental-block',
  templateUrl: './rental-block.component.html',
  styleUrl: './rental-block.component.scss'
})
export class RentalBlockComponent implements OnInit, OnDestroy {

  public rentalActive = false;
  public elapsed = '—';
  public pool = '—';
  public worker = '—';
  public shares = 0;
  public bestShare = '—';
  public networkDiff = '—';
  public blockState = 'None';
  public candidates = 0;
  public candidateHash = '';
  public duringRental = false;
  public feePercent = 2;
  public settlement = 'MANUAL';

  private timer?: any;

  constructor(private sys: SystemApiService) {}

  ngOnInit(): void {
    this.poll();
    this.timer = setInterval(() => this.poll(), 5000);
  }

  ngOnDestroy(): void {
    if (this.timer) {
      clearInterval(this.timer);
    }
  }

  public get hasCandidate(): boolean {
    return !!this.candidateHash && this.candidateHash.length === 64;
  }

  public get explorerUrl(): string {
    return 'https://mempool.space/block/' + this.candidateHash;
  }

  private poll(): void {
    this.sys.getInfo('').subscribe({
      next: (i: any) => {
        if (!i) { return; }
        this.rentalActive = !!i.rentalActive;
        this.elapsed = this.fmtDuration(i.rentalElapsedS || 0);
        this.pool = i.stratumURL || '—';
        this.worker = i.rentalWorker || i.stratumUser || '—';
        this.shares = i.rentalShares ?? 0;
        this.bestShare = this.human(i.rentalBestShare || 0);
        this.networkDiff = this.human(i.networkDifficulty || 0);
        this.blockState = i.blockState || 'None';
        this.candidates = i.blockCandidates ?? 0;
        this.candidateHash = i.blockCandidateHash || '';
        this.duringRental = !!i.blockDuringRental;
        this.feePercent = i.blockSuccessFeePercent ?? 2;
        this.settlement = i.blockSettlement || 'MANUAL';
      },
      error: () => { /* carte simplement inactive */ }
    });
  }

  private fmtDuration(s: number): string {
    if (!s || s < 0) { return '—'; }
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    return h > 0 ? `${h} h ${m} min` : `${m} min`;
  }

  private human(v: number): string {
    if (!v) { return '—'; }
    if (v >= 1e12) { return (v / 1e12).toFixed(2) + ' T'; }
    if (v >= 1e9) { return (v / 1e9).toFixed(2) + ' G'; }
    if (v >= 1e6) { return (v / 1e6).toFixed(2) + ' M'; }
    if (v >= 1e3) { return (v / 1e3).toFixed(1) + ' k'; }
    return Math.round(v).toString();
  }
}
