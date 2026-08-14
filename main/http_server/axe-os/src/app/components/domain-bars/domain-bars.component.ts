import { Component, Input } from '@angular/core';

@Component({
  selector: 'app-domain-bars',
  templateUrl: './domain-bars.component.html',
  styleUrl: './domain-bars.component.scss'
})
export class DomainBarsComponent {

  @Input() asic: any;
  @Input() index = 0;
  @Input() showLabel = false;

  get domains(): number[] {
    return (this.asic && Array.isArray(this.asic.domains)) ? this.asic.domains : [];
  }

  get total(): number {
    return this.domains.reduce((a, b) => a + b, 0);
  }

  get mean(): number {
    const d = this.domains;
    return d.length ? this.total / d.length : 0;
  }

  get max(): number {
    return this.domains.length ? Math.max(...this.domains) : 1;
  }

  get spread(): number {
    const d = this.domains;
    if (d.length < 2) {
      return 0;
    }
    const mx = Math.max(...d);
    const mn = Math.min(...d);
    return mx ? Math.round((mx - mn) / mx * 100) : 0;
  }

  public fill(domain: number): number {
    return Math.round(domain / (this.max || 1) * 100);
  }

  // Vert = le plus fort, ambre = décroche nettement (< -10% de la moyenne), sinon cyan neutre.
  public color(domain: number): string {
    const m = this.mean || 1;
    if (domain === this.max) {
      return '#41d68a';
    }
    if ((domain - m) / m < -0.10) {
      return '#f0b23c';
    }
    return '#35d0c0';
  }

  public flag(domain: number): string {
    const m = this.mean || 1;
    if (domain === this.max) {
      return 'FORT';
    }
    if ((domain - m) / m < -0.10) {
      return 'BAS';
    }
    return '';
  }
}
