clear; close all; clc;

cartella = fullfile(getenv('HOME'), 'Desktop', 'campagna dati');

files = { ...
    '433/senza ostacoli/gfskNoOb.csv'
    '433/senza ostacoli/gfskNoObCA.csv'
    '433/senza ostacoli/2fskNoOb.csv'
    '433/senza ostacoli/4fskNoOb.csv'
    '433/senza ostacoli/mskNoOb.csv'
    '433/senza ostacoli/ookNoOb.csv'
    '433/con ostacoli/gfskObCA.csv'
    '433/con ostacoli/gfskRIS.csv'
    '433/con ostacoli/gsfkiMac.csv'
    '433/con ostacoli/gsfkCartone.csv'
    '2,4/senza ostacoli/gfskNoObCA.csv'
    '2,4/senza ostacoli/gfskNoOb3.csv'
    '2,4/senza ostacoli/gfskNoOb4.csv'
    '2,4/con ostacoli/gfskCALatta.csv'
    '2,4/con ostacoli/gfskCAStagnola.csv'
    '2,4/con ostacoli/gfskNoObCACappelloTx.csv'
    '2,4/con ostacoli/gfskNoObCACappelloRx.csv'
    '2,4/con ostacoli/gfskOB.csv'
    };

opt.livello     = 0.95;
opt.k_blocchi   = 20;
opt.usa_blocchi = true;
opt.soglia_mad  = 6;
opt.n_pathloss  = 2.65;
opt.n_anecoica  = 2;
opt.nbins       = 40;

stile.titoli    = false;
stile.font      = 'Times New Roman';
stile.fontsize  = 11;
stile.dim_cm    = [14 10.5];
stile.mostra    = false;
stile.blu       = [0.00 0.30 0.60];
stile.rosso     = [0.75 0.15 0.10];
stile.verde     = [0.20 0.50 0.20];
stile.grigio    = [0.55 0.55 0.55];

c = 299792458;
outroot = fullfile(cartella, 'figure');
T_pos_tutte = table(); T_dist_tutte = table();

for f = 1:numel(files)
    percorso = fullfile(cartella, files{f});
    [~, nome] = fileparts(percorso);
    banda = strtok(files{f}, '/');
    if strcmp(banda, '2,4'); etichetta_banda = '2.4 GHz'; else; etichetta_banda = '433 MHz'; end
    titolo = sprintf('%s - %s', etichetta_banda, nome);
    fprintf('\n================ %s ================\n', titolo);

    prova  = [strrep(banda, ',', '') '_' nome];
    S = leggi_campagna(percorso);
    has_rssi = isfield(S.dati, 'rssi_dbm');

    if contains(nome, 'CA')
        n_pl = opt.n_anecoica;
    else
        n_pl = opt.n_pathloss;
    end
    if has_rssi; fprintf('Esponente di path loss: n = %.2f\n', n_pl); end

    d_distT = fullfile(outroot, prova, 'distribuzioni_ToF');
    d_probT = fullfile(outroot, prova, 'probabilita_ToF');
    d_distR = fullfile(outroot, prova, 'distribuzioni_RSSI');
    d_probR = fullfile(outroot, prova, 'probabilita_RSSI');
    d_stim  = fullfile(outroot, prova, 'stime');
    cartelle = {d_distT, d_probT, d_stim};
    if has_rssi; cartelle = [cartelle, {d_distR, d_probR}]; end
    cellfun(@crea_cartella, cartelle);

    cal = S.dati.fase == 0;
    tof_cal = pulisci(S.dati.tof_ds_ns(cal), opt.soglia_mad);
    m_cal = mean(tof_cal);
    fprintf('Calibrazione: M = %d   ToF medio = %.2f ns (costante)\n', numel(tof_cal), m_cal);
    if has_rssi
        P_cal = S.dati.rssi_dbm(cal);
        m_Pcal = mean(P_cal);
        fprintf('              RSSI medio = %.2f dBm (costante)\n', m_Pcal);
    end

    P = numel(S.piano.fase);
    R = struct('dvera', [], 'etichetta', {{}}, 'd', [], 'ic', [], ...
               'tof', {{}}, 'rssi', {{}}, 'N', [], 'n_out', [], 'mT', [], 'sT', [], ...
               'mP', [], 'sP', [], 'dP', [], 'dR', []);
    for p = 1:P
        sel = S.dati.fase == S.piano.fase(p);
        [tof, n_out] = pulisci(S.dati.tof_ds_ns(sel), opt.soglia_mad);
        [se2, nu] = varianza_media(tof, opt);

        delta = mean(tof) - m_cal;
        se    = sqrt(se2);
        t     = tinv_base(1 - (1-opt.livello)/2, nu);

        R.dvera(p)     = S.piano.dist(p);
        R.etichetta{p} = S.piano.label{p};
        R.d(p)         = S.d_cal + c*delta*1e-9;
        R.ic(p,:)      = R.d(p) + [-1 1] * t*se*c*1e-9;
        R.tof{p}       = tof;
        R.N(p)         = numel(tof);
        R.n_out(p)     = n_out;
        R.mT(p)        = mean(tof);
        R.sT(p)        = std(tof);

        fprintf('Pos %2d  %.2f m  %-16s  N = %d (outlier %d)\n', ...
            p, R.dvera(p), R.etichetta{p}, R.N(p), n_out);
        fprintf('        ToF : media = %9.2f ns  s = %6.1f ns  Delta = %7.2f ns  ->  d = %7.3f m  IC95 = [%7.3f ; %7.3f] m\n', ...
            R.mT(p), R.sT(p), delta, R.d(p), R.ic(p,1), R.ic(p,2));

        if has_rssi
            Pm = S.dati.rssi_dbm(sel);
            R.rssi{p}  = Pm;
            R.mP(p)    = mean(Pm);
            R.sP(p)    = std(Pm);
            R.dP(p)    = R.mP(p) - m_Pcal;
            R.dR(p)    = rssi2dist(R.dP(p), S.d_cal, n_pl);
            fprintf('        RSSI: media = %9.2f dBm s = %6.2f dB  dP    = %7.2f dB  ->  d = %7.3f m  (IC solo tra le posizioni)\n', ...
                R.mP(p), R.sP(p), R.dP(p), R.dR(p));
        end
    end

    dist_u = unique(R.dvera);
    A = struct('d', [], 'ic', [], 'P', [], 'dR', [], 'icR', []);
    fprintf('--- Stima per distanza (media sulle posizioni) ---\n');
    for j = 1:numel(dist_u)
        k  = R.dvera == dist_u(j);
        Pj = nnz(k);
        A.P(j) = Pj;
        A.d(j) = mean(R.d(k));
        if Pj > 1
            t = tinv_base(1-(1-opt.livello)/2, Pj-1);
            A.ic(j,:) = A.d(j) + [-1 1] * t*std(R.d(k))/sqrt(Pj);
        else
            A.ic(j,:) = R.ic(k,:);
        end
        fprintf('>>> %.2f m (%d pos.)  ToF : d = %7.3f m  IC95 = [%7.3f ; %7.3f] m  errore = %7.3f m\n', ...
            dist_u(j), Pj, A.d(j), A.ic(j,1), A.ic(j,2), A.d(j) - dist_u(j));
        if has_rssi
            dPm = mean(R.dP(k));
            if Pj > 1
                hw = tinv_base(1-(1-opt.livello)/2, Pj-1) * std(R.dP(k))/sqrt(Pj);
            else
                hw = NaN;
            end
            A.dR(j)    = rssi2dist(dPm, S.d_cal, n_pl);
            A.icR(j,:) = sort(rssi2dist(dPm + [-hw hw], S.d_cal, n_pl));
            fprintf('                     RSSI: d = %7.3f m  IC95 = [%7.3f ; %7.3f] m  errore = %7.3f m\n', ...
                A.dR(j), A.icR(j,1), A.icR(j,2), A.dR(j) - dist_u(j));
        end
    end

    nanv = nan(P, 1);
    Tpos = table(repmat({etichetta_banda}, P, 1), repmat({nome}, P, 1), ...
        R.dvera(:), R.etichetta(:), R.N(:), R.n_out(:), ...
        R.mT(:), R.sT(:), R.mT(:) - m_cal, R.d(:), R.ic(:,1), R.ic(:,2), R.d(:) - R.dvera(:), ...
        'VariableNames', {'banda', 'prova', 'd_vera_m', 'posizione', 'N', 'outlier', ...
        'ToF_medio_ns', 'ToF_s_ns', 'Delta_ns', 'd_ToF_m', 'IC95_ToF_inf_m', 'IC95_ToF_sup_m', 'errore_ToF_m'});
    if has_rssi
        Tpos.RSSI_medio_dBm = R.mP(:);  Tpos.RSSI_s_dB = R.sP(:);  Tpos.dP_dB = R.dP(:);
        Tpos.d_RSSI_m = R.dR(:);
        Tpos.errore_RSSI_m = R.dR(:) - R.dvera(:);
    else
        Tpos.RSSI_medio_dBm = nanv; Tpos.RSSI_s_dB = nanv; Tpos.dP_dB = nanv;
        Tpos.d_RSSI_m = nanv;
        Tpos.errore_RSSI_m = nanv;
    end

    J = numel(dist_u); nanj = nan(J, 1);
    Tdist = table(repmat({etichetta_banda}, J, 1), repmat({nome}, J, 1), dist_u(:), A.P(:), ...
        A.d(:), A.ic(:,1), A.ic(:,2), A.d(:) - dist_u(:), ...
        'VariableNames', {'banda', 'prova', 'd_vera_m', 'posizioni', ...
        'd_ToF_m', 'IC95_ToF_inf_m', 'IC95_ToF_sup_m', 'errore_ToF_m'});
    if has_rssi
        Tdist.d_RSSI_m = A.dR(:); Tdist.IC95_RSSI_inf_m = A.icR(:,1);
        Tdist.IC95_RSSI_sup_m = A.icR(:,2); Tdist.errore_RSSI_m = A.dR(:) - dist_u(:);
    else
        Tdist.d_RSSI_m = nanj; Tdist.IC95_RSSI_inf_m = nanj;
        Tdist.IC95_RSSI_sup_m = nanj; Tdist.errore_RSSI_m = nanj;
    end

    d_ris = fullfile(outroot, prova);
    writetable(Tpos,  fullfile(d_ris, 'risultati_posizioni.csv'));
    writetable(Tdist, fullfile(d_ris, 'risultati_distanze.csv'));
    scrivi_tabella_latex(fullfile(d_ris, 'tabella_distanze.tex'), Tdist, has_rssi, ...
        sprintf('Stime di distanza con IC al 95\\%% - %s, prova \\texttt{%s}.', ...
        etichetta_banda, strrep(nome, '_', '\_')), ['tab:' prova]);
    T_pos_tutte   = [T_pos_tutte;   Tpos];
    T_dist_tutte  = [T_dist_tutte;  Tdist];

    sovrapponi = true;
    lbl = [{sprintf('calibrazione a %.2f m', S.d_cal)}, ...
           arrayfun(@(p) sprintf('%.2f m, %s', R.dvera(p), R.etichetta{p}), 1:P, 'UniformOutput', false)];
    fn  = [{'00_calibrazione.pdf'}, ...
           arrayfun(@(p) nome_file(p, R.dvera(p), R.etichetta{p}), 1:P, 'UniformOutput', false)];
    datiT = [{tof_cal}, R.tof];
    if has_rssi; datiR = [{P_cal}, R.rssi]; end

    [fig, ax] = nuova_figura(stile);
    istogramma(ax, tof_cal, 'tof', opt.nbins, m_cal, true, stile);
    metti_titolo(ax, stile, sprintf('%s - ToF, %s', titolo, lbl{1}));
    salva(fig, fullfile(d_distT, fn{1}));
    if has_rssi
        [fig, ax] = nuova_figura(stile);
        istogramma(ax, P_cal, 'rssi', opt.nbins, m_Pcal, true, stile);
        metti_titolo(ax, stile, sprintf('%s - RSSI, %s', titolo, lbl{1}));
        salva(fig, fullfile(d_distR, fn{1}));
    end
    if sovrapponi
        for j = 1:numel(dist_u)
            k = find(R.dvera == dist_u(j));
            fnd = sprintf('%02d_%.2fm_posizioni.pdf', j, dist_u(j));
            [fig, ax] = nuova_figura(stile);
            istogramma_multi(ax, R.tof(k), R.etichetta(k), 'tof', opt.nbins, m_cal, stile);
            metti_titolo(ax, stile, sprintf('%s - ToF, %.2f m', titolo, dist_u(j)));
            salva(fig, fullfile(d_distT, fnd));
            if has_rssi
                [fig, ax] = nuova_figura(stile);
                istogramma_multi(ax, R.rssi(k), R.etichetta(k), 'rssi', opt.nbins, m_Pcal, stile);
                metti_titolo(ax, stile, sprintf('%s - RSSI, %.2f m', titolo, dist_u(j)));
                salva(fig, fullfile(d_distR, fnd));
            end
        end
    else
        for q = 2:numel(datiT)
            [fig, ax] = nuova_figura(stile);
            istogramma(ax, datiT{q}, 'tof', opt.nbins, m_cal, false, stile);
            metti_titolo(ax, stile, sprintf('%s - ToF, %s', titolo, lbl{q}));
            salva(fig, fullfile(d_distT, fn{q}));
            if has_rssi
                [fig, ax] = nuova_figura(stile);
                istogramma(ax, datiR{q}, 'rssi', opt.nbins, m_Pcal, false, stile);
                metti_titolo(ax, stile, sprintf('%s - RSSI, %s', titolo, lbl{q}));
                salva(fig, fullfile(d_distR, fn{q}));
            end
        end
    end

    for q = 1:numel(datiT)
        [fig, ax] = nuova_figura(stile);
        grafico_probabilita(ax, datiT{q}, '$x_i$ (tempo di volo) [ns]', stile);
        metti_titolo(ax, stile, sprintf('%s - ToF, %s', titolo, lbl{q}));
        salva(fig, fullfile(d_probT, fn{q}));
        if has_rssi
            [fig, ax] = nuova_figura(stile);
            grafico_probabilita(ax, datiR{q}, '$x_i$ (RSSI) [dBm]', stile);
            metti_titolo(ax, stile, sprintf('%s - RSSI, %s', titolo, lbl{q}));
            salva(fig, fullfile(d_probR, fn{q}));
        end
    end

    [fig, ax] = nuova_figura(stile);
    if numel(dist_u) > 1
        off = zeros(1, P);
        for j = 1:numel(dist_u)
            k = find(R.dvera == dist_u(j));
            off(k) = linspace(-0.06, 0.06, numel(k));
        end
        x = R.dvera + off;
        lim = [min(dist_u)-0.5, max(dist_u)+0.5];
        h(1) = plot(ax, lim, lim, '--', 'Color', 'k', 'LineWidth', 0.8);
        h(2) = errorbar(ax, x, R.d, R.d - R.ic(:,1)', R.ic(:,2)' - R.d, 'o', ...
            'Color', stile.grigio, 'MarkerSize', 4, 'MarkerFaceColor', 'w', 'CapSize', 3);
        h(3) = errorbar(ax, dist_u, A.d, A.d - A.ic(:,1)', A.ic(:,2)' - A.d, 's', ...
            'Color', stile.blu, 'LineWidth', 1.3, 'MarkerSize', 7, ...
            'MarkerFaceColor', stile.blu, 'CapSize', 6);
        xlim(ax, lim);
        xlabel(ax, 'Distanza reale [m]');
        legend(ax, h, {'Stima ideale', 'Singola posizione (IC 95\%)', ...
            'Media sulle posizioni (IC 95\%)'}, 'Location', 'best');
    else
        x = 1:P;
        h(1) = patch(ax, [0.5 P+0.5 P+0.5 0.5], [A.ic(1) A.ic(1) A.ic(2) A.ic(2)], ...
            stile.rosso, 'FaceAlpha', 0.12, 'EdgeColor', 'none');
        h(2) = plot(ax, [0.5 P+0.5], [A.d A.d], '-', 'Color', stile.rosso, 'LineWidth', 1.2);
        h(3) = plot(ax, [0.5 P+0.5], [dist_u dist_u], '--', 'Color', 'k', 'LineWidth', 0.8);
        h(4) = errorbar(ax, x, R.d, R.d - R.ic(:,1)', R.ic(:,2)' - R.d, 'o', ...
            'Color', stile.blu, 'LineWidth', 1.1, 'MarkerSize', 6, ...
            'MarkerFaceColor', stile.blu, 'CapSize', 5);
        set(ax, 'XTick', x, 'XTickLabel', strrep(R.etichetta, 'x = ', ''));
        xlim(ax, [0.5 P+0.5]);
        xlabel(ax, 'Spostamento laterale del nodo B');
        legend(ax, h([4 2 1 3]), {'Singola posizione (IC 95\%)', ...
            sprintf('Media sulle posizioni: %.2f m', A.d), ...
            'IC 95\% della media', 'Distanza reale'}, 'Location', 'best');
    end
    ylabel(ax, 'Distanza stimata (ToF) [m]');
    metti_titolo(ax, stile, ['Stima ToF - ' titolo]);
    salva(fig, fullfile(d_stim, 'stima_ToF.pdf'));
    clear h

    if has_rssi
        dR_pos = rssi2dist(R.dP, S.d_cal, n_pl);
        [fig, ax] = nuova_figura(stile);
        lim = [0, max(dist_u)+0.5];
        h(1) = plot(ax, lim, lim, '--', 'Color', 'k', 'LineWidth', 0.8);
        h(2) = plot(ax, x, dR_pos, 'o', 'Color', stile.grigio, 'MarkerSize', 4, ...
            'MarkerFaceColor', 'w');
        h(3) = errorbar(ax, dist_u, A.dR, A.dR - A.icR(:,1)', A.icR(:,2)' - A.dR, 's', ...
            'Color', stile.verde, 'LineWidth', 1.3, 'MarkerSize', 7, ...
            'MarkerFaceColor', stile.verde, 'CapSize', 6);
        xlim(ax, lim); ylim(ax, lim);
        xlabel(ax, 'Distanza reale [m]');
        ylabel(ax, 'Distanza stimata (RSSI) [m]');
        legend(ax, h, {'Stima ideale', 'Singola posizione', ...
            sprintf('Media sulle posizioni (IC 95\\%%), $n = %.2f$', n_pl)}, ...
            'Location', 'best');
        metti_titolo(ax, stile, ['Stima RSSI - ' titolo]);
        salva(fig, fullfile(d_stim, 'stima_RSSI.pdf'));
        clear h
    end
end
writetable(T_pos_tutte,  fullfile(outroot, 'risultati_posizioni_tutte.csv'));
writetable(T_dist_tutte, fullfile(outroot, 'risultati_distanze_tutte.csv'));
fprintf('\nFigure e risultati salvati in: %s\n', outroot);

function S = leggi_campagna(percorso)
    testo = fileread(percorso);
    righe = regexp(testo, '\r?\n', 'split');

    tok = regexp(testo, 'D_CAL\s*=\s*([\d.]+)', 'tokens', 'once');
    S.d_cal = str2double(tok{1});

    pp = regexp(testo, '#PIANO,(\d+),([\d.]+),([^\r\n]*)', 'tokens');
    S.piano.fase  = cellfun(@(t) str2double(t{1}), pp);
    S.piano.dist  = cellfun(@(t) str2double(t{2}), pp);
    S.piano.label = cellfun(@(t) strtrim(t{3}), pp, 'UniformOutput', false);
    ok = S.piano.dist > 0 & ~strcmpi(S.piano.label, 'libera');
    S.piano.fase  = S.piano.fase(ok);
    S.piano.dist  = S.piano.dist(ok);
    S.piano.label = S.piano.label(ok);

    dati = righe(~startsWith(righe, '#') & strlength(righe) > 0);
    nomi = strsplit(dati{1}, ',');
    dati = dati(2:end);
    ok = cellfun(@(r) nnz(r == ','), dati) == numel(nomi) - 1;
    M = str2double(split(string(dati(ok))', ','));
    for k = 1:numel(nomi)
        S.dati.(strtrim(nomi{k})) = M(:, k);
    end
end

function [x, n_out] = pulisci(x, soglia)
    x = x(~isnan(x));
    med = median(x);
    mad_ = 1.4826 * median(abs(x - med));
    ok = abs(x - med) <= soglia * mad_;
    n_out = nnz(~ok);
    x = x(ok);
end

function [se2, nu] = varianza_media(x, opt)
    x = x(:);
    N = numel(x);
    if opt.usa_blocchi && N >= 2*opt.k_blocchi
        k = opt.k_blocchi;
        L = floor(N / k);
        mb = mean(reshape(x(1:k*L), L, k), 1);
        se2 = var(mb) / k;
        nu  = k - 1;
    else
        se2 = var(x) / N;
        nu  = N - 1;
    end
end

function d = rssi2dist(dP, d0, n)
    d = d0 * 10.^(-dP ./ (10*n));
end

function t = tinv_base(p, nu)
    if isinf(nu) || nu > 1e6
        t = sqrt(2) * erfinv(2*p - 1);
        return
    end
    q = 2 * min(p, 1-p);
    x = betaincinv(q, nu/2, 0.5);
    t = sign(p - 0.5) * sqrt(nu * (1./x - 1));
end

function scrivi_tabella_latex(file, T, has_rssi, caption, label)
    fid = fopen(file, 'w');
    fprintf(fid, '%% Generata da analisi_distanze.m - richiede \\usepackage{booktabs}\n');
    fprintf(fid, '\\begin{table}[htbp]\n\\centering\n');
    if has_rssi
        fprintf(fid, '\\begin{tabular}{cc ccc ccc}\n\\toprule\n');
        fprintf(fid, ' & & \\multicolumn{3}{c}{ToF} & \\multicolumn{3}{c}{RSSI} \\\\\n');
        fprintf(fid, '\\cmidrule(lr){3-5} \\cmidrule(lr){6-8}\n');
        fprintf(fid, '$d$ [m] & $P$ & $\\hat{d}$ [m] & IC 95\\%% [m] & $\\varepsilon$ [m] & $\\hat{d}$ [m] & IC 95\\%% [m] & $\\varepsilon$ [m] \\\\\n\\midrule\n');
        for r = 1:height(T)
            if isnan(T.IC95_RSSI_inf_m(r))
                icR = '--';
            else
                icR = sprintf('$[%.2f;\\ %.2f]$', T.IC95_RSSI_inf_m(r), T.IC95_RSSI_sup_m(r));
            end
            fprintf(fid, '%.2f & %d & %.2f & $[%.2f;\\ %.2f]$ & %+.2f & %.2f & %s & %+.2f \\\\\n', ...
                T.d_vera_m(r), T.posizioni(r), T.d_ToF_m(r), T.IC95_ToF_inf_m(r), T.IC95_ToF_sup_m(r), ...
                T.errore_ToF_m(r), T.d_RSSI_m(r), icR, T.errore_RSSI_m(r));
        end
    else
        fprintf(fid, '\\begin{tabular}{cc ccc}\n\\toprule\n');
        fprintf(fid, '$d$ [m] & $P$ & $\\hat{d}_{\\mathrm{ToF}}$ [m] & IC 95\\%% [m] & $\\varepsilon$ [m] \\\\\n\\midrule\n');
        for r = 1:height(T)
            fprintf(fid, '%.2f & %d & %.2f & $[%.2f;\\ %.2f]$ & %+.2f \\\\\n', ...
                T.d_vera_m(r), T.posizioni(r), T.d_ToF_m(r), T.IC95_ToF_inf_m(r), ...
                T.IC95_ToF_sup_m(r), T.errore_ToF_m(r));
        end
    end
    fprintf(fid, '\\bottomrule\n\\end{tabular}\n');
    fprintf(fid, '\\caption{%s}\n\\label{%s}\n\\end{table}\n', caption, label);
    fclose(fid);
end

function [fig, ax] = nuova_figura(stile)
    vis = 'off'; if stile.mostra; vis = 'on'; end
    fig = figure('Visible', vis, 'Color', 'w', 'Units', 'centimeters', ...
                 'Position', [2 2 stile.dim_cm], 'PaperUnits', 'centimeters', ...
                 'PaperSize', stile.dim_cm);
    try, theme(fig, 'light'); catch, end
    ax = axes(fig);
    hold(ax, 'on');
    set(ax, 'Color', 'w', 'XColor', 'k', 'YColor', 'k', ...
        'FontName', stile.font, 'FontSize', stile.fontsize, ...
        'TickLabelInterpreter', 'latex', 'LineWidth', 0.6, ...
        'Box', 'on', 'TickDir', 'in', 'TickLength', [0.012 0.012], ...
        'XGrid', 'on', 'YGrid', 'on', 'GridColor', [0.80 0.80 0.80], ...
        'GridAlpha', 1, 'GridLineStyle', ':', 'Layer', 'top');
    set(fig, 'DefaultTextInterpreter', 'latex', ...
             'DefaultLegendInterpreter', 'latex', ...
             'DefaultAxesTickLabelInterpreter', 'latex');
    ax.XLabel.Interpreter = 'latex'; ax.YLabel.Interpreter = 'latex';
    ax.Title.Interpreter  = 'latex';
    ax.XLabel.Color = 'k'; ax.YLabel.Color = 'k'; ax.Title.Color = 'k';
end

function metti_titolo(ax, stile, testo)
    if stile.titoli
        title(ax, strrep(testo, '_', '\_'), 'FontWeight', 'normal');
    end
end

function salva(fig, file)
    lg = findobj(fig, 'Type', 'Legend');
    for k = 1:numel(lg)
        set(lg(k), 'Color', 'w', 'TextColor', 'k', 'EdgeColor', [0.3 0.3 0.3], ...
            'Interpreter', 'latex', 'Location', 'southoutside', ...
            'Orientation', 'horizontal');
        lunghe = max(cellfun(@strlength, lg(k).String)) > 45;
        lg(k).NumColumns = 1 + ~lunghe * (numel(lg(k).String) > 1);
    end
    exportgraphics(fig, file, 'ContentType', 'vector', 'BackgroundColor', 'white');
    close(fig);
end

function crea_cartella(d)
    if ~exist(d, 'dir'); mkdir(d); end
end

function s = nome_file(p, dv, lab)
    lab = regexprep(lab, '[^\w+-]', '');
    lab = strrep(lab, '+', 'p');
    s = sprintf('%02d_%.2fm_%s.pdf', p, dv, lab);
end

function istogramma(ax, x, tipo, nbins, m_cal, is_cal, stile)
    x = x(:);
    if strcmp(tipo, 'rssi')
        passo = min(diff(unique(x)));
        if isempty(passo) || passo == 0; passo = 0.5; end
        edges = (min(x) - passo/2) : passo : (max(x) + passo/2);
        histogram(ax, x, edges, 'Normalization', 'pdf', ...
            'FaceColor', [0.85 0.90 0.84], 'EdgeColor', [0.40 0.50 0.40], 'LineWidth', 0.4);
        unita = 'dBm'; xl = 'RSSI [dBm]'; yl = 'pdf, $f_i$ [dB$^{-1}$]';
        fmt_m = '%.2f'; fmt_s = '%.2f';
    else
        histogram(ax, x, nbins, 'Normalization', 'pdf', ...
            'FaceColor', [0.82 0.86 0.92], 'EdgeColor', [0.45 0.50 0.60], 'LineWidth', 0.4);
        unita = 'ns'; xl = 'Tempo di volo (DS-TWR) [ns]'; yl = 'pdf, $f_i$ [ns$^{-1}$]';
        fmt_m = '%.1f'; fmt_s = '%.1f';
    end
    mu = mean(x); s = std(x);
    if s > 0
        xx = linspace(min(x) - s, max(x) + s, 400);
        plot(ax, xx, exp(-(xx-mu).^2/(2*s^2)) / (s*sqrt(2*pi)), '-', ...
            'Color', stile.rosso, 'LineWidth', 1.4);
    end
    voci = {'Dati sperimentali', sprintf(['Gaussiana ($\\bar{x} = ' fmt_m '$ %s, $s = ' fmt_s '$ %s)'], ...
            mu, unita, s, unita)};
    if ~is_cal
        xline(ax, m_cal, '--', 'Color', 'k', 'LineWidth', 1.0);
        voci{end+1} = sprintf(['Media di calibrazione (' fmt_m ' %s)'], m_cal, unita);
    end
    xlabel(ax, xl);
    ylabel(ax, yl);
    legend(ax, voci, 'Location', 'northeast');
end

function istogramma_multi(ax, dati, etichette, tipo, nbins, m_cal, stile)
    pal = [0.00 0.45 0.74;  0.85 0.33 0.10;  0.47 0.67 0.19;  0.49 0.18 0.56; ...
           0.93 0.69 0.13;  0.30 0.75 0.93;  0.64 0.08 0.18;  0.40 0.40 0.40];
    tutti = vertcat(dati{:});
    if strcmp(tipo, 'rssi')
        passo = min(diff(unique(tutti)));
        if isempty(passo) || passo == 0; passo = 0.5; end
        edges = (min(tutti) - passo/2) : passo : (max(tutti) + passo/2);
        unita = 'dBm'; xl = 'RSSI [dBm]'; yl = 'pdf, $f_i$ [dB$^{-1}$]'; fmt = '%.2f';
    else
        edges = linspace(min(tutti), max(tutti), nbins + 1);
        unita = 'ns'; xl = 'Tempo di volo (DS-TWR) [ns]'; yl = 'pdf, $f_i$ [ns$^{-1}$]'; fmt = '%.1f';
    end
    h = gobjects(0); voci = {};
    for k = 1:numel(dati)
        col = pal(mod(k-1, size(pal,1)) + 1, :);
        x = dati{k}(:);
        if numel(dati) <= 4
            histogram(ax, x, edges, 'Normalization', 'pdf', 'FaceColor', col, ...
                'FaceAlpha', 0.25, 'EdgeColor', col, 'EdgeAlpha', 0.5, 'LineWidth', 0.4);
        end
        mu = mean(x); s = std(x);
        if s > 0
            xx = linspace(edges(1) - s, edges(end) + s, 400);
            h(end+1) = plot(ax, xx, exp(-(xx-mu).^2/(2*s^2)) / (s*sqrt(2*pi)), '-', ...
                'Color', col, 'LineWidth', 1.4);
        else
            h(end+1) = plot(ax, NaN, NaN, '-', 'Color', col, 'LineWidth', 1.4);
        end
        voci{end+1} = sprintf(['%s ($\\bar{x} = ' fmt '$, $s = ' fmt '$ %s)'], ...
            etichette{k}, mu, s, unita);
    end
    h(end+1) = xline(ax, m_cal, '--', 'Color', 'k', 'LineWidth', 1.0);
    voci{end+1} = sprintf(['Media di calibrazione (' fmt ' %s)'], m_cal, unita);
    xlabel(ax, xl);
    ylabel(ax, yl);
    legend(ax, h, voci);
end

function grafico_probabilita(ax, x, etichetta_x, stile)
    x = sort(x(:));
    n = numel(x);
    u = sqrt(2) * erfinv(2*((1:n)'/(n+1)) - 1);
    idx = unique(round(linspace(1, n, min(n, 800))));
    if std(x) == 0; return; end
    pfit = polyfit(x, u, 1);
    rho = corrcoef(x, u); rho = rho(1, 2);
    plot(ax, x(idx), u(idx), '.', 'MarkerSize', 6, 'Color', stile.blu);
    plot(ax, x([1 end]), polyval(pfit, x([1 end])), '-', 'Color', stile.rosso, 'LineWidth', 1.2);
    xlabel(ax, etichetta_x);
    ylabel(ax, '$F_U^{-1}\left(\frac{i}{n+1}\right)$');
    legend(ax, {'Dati sperimentali', sprintf('Retta di regressione ($\\rho = %.3f$)', rho)}, ...
        'Location', 'southeast');
end
