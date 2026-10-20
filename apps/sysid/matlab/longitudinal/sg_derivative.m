function d = sg_derivative(x, t, frameLen)
%SG_DERIVATIVE Savitzky-Golay first derivative of x(t) (order 2).
%   The recordings are close to 60 Hz; the median sample time is used as the step.
%   Samples whose window crosses a time gap larger than 3 steps are set to NaN.
arguments
    x (:,1) double
    t (:,1) double
    frameLen (1,1) double = 9
end
h = median(diff(t));
[~, g] = sgolay(2, frameLen);
d = conv(x, factorial(1) / (-h) * g(:, 2), "same");
half = (frameLen - 1) / 2;
d([1:half, end-half+1:end]) = NaN;
gap = find(diff(t) > 3 * h);
for k = gap(:).'
    d(max(1, k-half):min(numel(d), k+1+half)) = NaN;
end
end
